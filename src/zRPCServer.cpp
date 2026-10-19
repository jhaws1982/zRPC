/*
 * @file   zRPCServer.cpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 3:09:39 pm
 *
 * @brief 0MQ-based RPC client/server library with MessagePack support
 *
 * @copyright Jonathan Haws -- 2021
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "zRPC.hpp"

#include <deque>
#include <unordered_map>

namespace zRPC
{

namespace
{
/**
 * @brief Server whose worker is running on this thread, if any
 */
thread_local const Server *t_worker = nullptr;

/**
 * @brief Set when a bound RPC running on this thread asked its server to stop
 */
thread_local bool t_stopAfterReply = false;

/**
 * @brief Control message a worker sends the broker when it is ready for work
 */
constexpr char kReady[] = "READY";

/**
 * @brief Control message a worker sends the broker after replying to an RPC
 * that called stop()
 */
constexpr char kStop[] = "STOP";

/**
 * @brief The broker's record of calls by request ID, which makes each call
 * run at most once however often it is resent. Used only by the broker
 * thread.
 */
class CallMemory
{
public:
  /**
   * @brief What the broker should do with a request
   */
  enum class Action
  {
    /**
     * @brief A new call: hand it to a worker
     */
    Run,

    /**
     * @brief Already running: the client gets that run's reply
     */
    Wait,

    /**
     * @brief Already finished: send the stored reply()
     */
    Replay,

    /**
     * @brief Already finished, but its reply was dropped to stay within the
     * byte budget: say so, without running the call again
     */
    Unavailable,

    /**
     * @brief The ID belongs to a different call
     */
    Conflict,
  };

  CallMemory(std::chrono::milliseconds window,
             std::size_t maxCalls,
             std::size_t maxBytes) :
      m_window(window),
      m_maxCalls(maxCalls),
      m_maxBytes(maxBytes)
  {
  }

  /**
   * @brief Note a request arriving from a client connection
   *
   * @param[in] request ID and CRC of the request
   * @param[in] client Routing ID of the connection it arrived on
   * @return Action What to do with the request
   */
  Action arrive(const detail::Peeked &request, zmq::message_t &client)
  {
    auto [it, isNew] = m_calls.try_emplace(request.id);
    Call &call = it->second;
    if (isNew)
    {
      call.crc = request.crc;
    }
    else if (call.crc != request.crc)
    {
      // The same call always has the same bytes, and so the same CRC
      return Action::Conflict;
    }
    else if (call.finished)
    {
      return call.replyKept ? Action::Replay : Action::Unavailable;
    }

    // Reply to each connection once, however often it resends
    for (const auto &waiting : call.waiting)
    {
      if (waiting == client)
      {
        return Action::Wait;
      }
    }
    call.waiting.emplace_back();
    call.waiting.back().copy(client);
    return isNew ? Action::Run : Action::Wait;
  }

  /**
   * @brief Forget a call that was never run, e.g. because no worker could
   * take it
   *
   * @param[in] id Request ID of the call
   */
  void abandon(const RequestId &id)
  {
    const auto it = m_calls.find(id);
    if (it != m_calls.end() && !it->second.finished)
    {
      m_calls.erase(it);
    }
  }

  /**
   * @brief Stored reply of a finished call whose arrive() returned Replay
   */
  const std::string &reply(const RequestId &id) const
  {
    return m_calls.at(id).reply;
  }

  /**
   * @brief Note a worker's reply to a call
   *
   * @param[in] id Request ID the reply echoes
   * @param[in] reply The reply
   * @param[in] now Current time
   * @return std::vector<zmq::message_t> Every connection waiting for the
   * reply, or none if the call was not remembered
   */
  std::vector<zmq::message_t> finish(const RequestId &id,
                                     const zmq::message_t &reply,
                                     std::chrono::steady_clock::time_point now)
  {
    auto it = m_calls.find(id);
    if (it == m_calls.end() || it->second.finished)
    {
      return {};
    }

    auto waiting = std::move(it->second.waiting);
    if (m_window == std::chrono::milliseconds::zero())
    {
      m_calls.erase(it);
      return waiting;
    }

    Call &call = it->second;
    call.finished = true;
    call.waiting.clear();
    call.reply.assign(static_cast<const char *>(reply.data()), reply.size());
    call.replyKept = true;
    call.generation = ++m_generation;
    m_bytes += call.reply.size();
    m_finished.push_back({now, id, call.generation});
    m_kept.push_back({now, id, call.generation});
    dropRepliesOverBudget();
    return waiting;
  }

  /**
   * @brief Forget finished calls older than the window, and the oldest ones
   * beyond the limit
   *
   * @param[in] now Current time
   */
  void expire(std::chrono::steady_clock::time_point now)
  {
    while (!m_finished.empty() &&
           (detail::addSaturated(m_finished.front().at, m_window) <= now ||
            m_finished.size() > m_maxCalls))
    {
      if (detail::addSaturated(m_finished.front().at, m_window) > now &&
          !m_warnedCalls)
      {
        m_warnedCalls = true;
        detail::log(LogLevel::Warning,
                    "Server finished more than " + std::to_string(m_maxCalls) +
                        " calls within its memory window and is forgetting "
                        "the oldest early; a late resend of one could run it "
                        "again (see Server::rememberCalls())");
      }

      const auto it = find(m_finished.front());
      if (it != m_calls.end())
      {
        m_bytes -= it->second.reply.size();
        m_calls.erase(it);
      }
      m_finished.pop_front();
    }
  }

private:
  /**
   * @brief A call the broker has seen
   */
  struct Call
  {
    bool finished{false};

    /**
     * @brief Whether reply still holds the reply
     */
    bool replyKept{false};

    /**
     * @brief Distinguishes this call from an earlier one with the same ID
     * that has been forgotten
     */
    std::uint64_t generation{0};

    /**
     * @brief CRC of the request, which identifies its exact contents
     */
    std::uint32_t crc{0};

    /**
     * @brief Routing IDs of the connections waiting for the reply
     */
    std::vector<zmq::message_t> waiting;

    /**
     * @brief The reply, once finished, while it is kept
     */
    std::string reply;
  };

  /**
   * @brief A finished call, in the order calls finished
   */
  struct Finished
  {
    std::chrono::steady_clock::time_point at;
    RequestId id;
    std::uint64_t generation;
  };

  /**
   * @brief The call an entry refers to, if it is still remembered
   */
  std::unordered_map<RequestId, Call, detail::RequestIdHash>::iterator find(
      const Finished &entry)
  {
    auto it = m_calls.find(entry.id);
    if (it != m_calls.end() && it->second.generation != entry.generation)
    {
      return m_calls.end();
    }
    return it;
  }

  /**
   * @brief Drop the oldest stored replies while they take more than the
   * byte budget. Their calls stay remembered as finished, so a resend learns
   * that the reply is gone instead of running the call again.
   */
  void dropRepliesOverBudget()
  {
    while (m_bytes > m_maxBytes && !m_kept.empty())
    {
      const auto it = find(m_kept.front());
      m_kept.pop_front();
      if (it == m_calls.end() || !it->second.replyKept)
      {
        continue;
      }

      if (!m_warnedBytes)
      {
        m_warnedBytes = true;
        detail::log(LogLevel::Warning,
                    "Server's stored replies exceed " +
                        std::to_string(m_maxBytes) +
                        " bytes; dropping the oldest. A resend of one of "
                        "those calls gets a ReplyUnavailable error (see "
                        "Server::rememberCalls())");
      }
      m_bytes -= it->second.reply.size();
      std::string().swap(it->second.reply);
      it->second.replyKept = false;
    }
  }

  std::chrono::milliseconds m_window;
  std::size_t m_maxCalls;
  std::size_t m_maxBytes;
  std::unordered_map<RequestId, Call, detail::RequestIdHash> m_calls;

  /**
   * @brief Finished calls, oldest first, for expiry
   */
  std::deque<Finished> m_finished;

  /**
   * @brief Finished calls whose replies may still be kept, oldest first
   */
  std::deque<Finished> m_kept;

  /**
   * @brief Bytes of replies kept
   */
  std::size_t m_bytes{0};

  std::uint64_t m_generation{0};
  bool m_warnedCalls{false};
  bool m_warnedBytes{false};
};

/**
 * @brief Hand a request to the longest-idle worker that is still running
 *
 * The backend is a ROUTER_MANDATORY socket, so sending to a worker that has
 * stopped fails with EHOSTUNREACH; that worker is dropped and the next idle
 * one tried.
 *
 * @param[in] backend Broker's backend socket
 * @param[in,out] idle Routing IDs of idle workers, longest idle first
 * @param[in] client Routing ID of the client connection
 * @param[in] request The request
 * @return bool False if no idle worker was still running
 */
bool dispatch(zmq::socket_t &backend,
              std::deque<zmq::message_t> &idle,
              zmq::message_t &client,
              zmq::message_t &request)
{
  while (!idle.empty())
  {
    try
    {
      (void)backend.send(idle.front(), zmq::send_flags::sndmore);
    }
    catch (const zmq::error_t &e)
    {
      if (e.num() != EHOSTUNREACH)
      {
        throw;
      }
      idle.pop_front();
      continue;
    }
    idle.pop_front();
    (void)backend.send(client, zmq::send_flags::sndmore);
    (void)backend.send(request, zmq::send_flags::none);
    return true;
  }
  return false;
}

/**
 * @brief Request ID of a raw request, if it has one
 *
 * @param[in] msg Request as received
 * @return std::optional<RequestId> The ID, unverified
 */
std::optional<RequestId> requestId(const zmq::message_t &msg)
{
  if (const auto peeked = detail::peek(msg.data(), msg.size()))
  {
    return peeked->id;
  }
  return std::nullopt;
}

/**
 * @brief Build a reply carrying a zRPC::Error
 *
 * @param[in] code Category of the error
 * @param[in] msg Error message for the client
 * @param[in] id Request ID to echo, if the request had one
 * @return msgpack::sbuffer Reply envelope to send back
 */
msgpack::sbuffer errorReply(ErrorCode code,
                            const std::string &msg,
                            const std::optional<RequestId> &id)
{
  Error err;
  err.m_msg = msg;
  err.m_code = code;
  return detail::seal(detail::Status::Error, err, id);
}
}  // namespace

Server::Server(const uint16_t port, const uint32_t nWorkers) :
    Server("tcp://*:" + std::to_string(port), nWorkers)
{
}

Server::Server(const std::string &uri, const uint32_t nWorkers) :
    m_ctx(16),
    m_brokerFrontend(m_ctx, zmq::socket_type::router),
    m_brokerBackend(m_ctx, zmq::socket_type::router),
    m_nWorkers(nWorkers)
{
  // Start the broker sockets and bind to their ports. Peers sending larger
  // messages are disconnected by 0MQ before the data reaches a worker. A bind
  // failure (e.g. address in use) throws, before any worker thread exists.
  m_brokerFrontend.set(zmq::sockopt::maxmsgsize, detail::kMaxMessageSize);
  // Final replies get a grace period to reach their clients when the server
  // is destroyed, but a stalled client cannot hang the destructor
  m_brokerFrontend.set(zmq::sockopt::linger,
                       detail::zmqTimeout(detail::kLinger));
  m_brokerBackend.set(zmq::sockopt::linger,
                      detail::zmqTimeout(detail::kLinger));
  m_brokerFrontend.bind(uri);
  m_brokerBackend.bind("inproc://backend");
  // Sending to a worker that has stopped fails instead of silently
  // discarding the request, so the broker can skip that worker
  m_brokerBackend.set(zmq::sockopt::router_mandatory, true);
  m_endpoint = m_brokerFrontend.get(zmq::sockopt::last_endpoint);

  m_running = true;
}

Server::~Server()
{
  // Ensure we have shut things down completely
  stop();

  // Loop over all worker threads and join them
  for (auto &t : m_th)
  {
    if (t.joinable())
    {
      t.join();
    }
  }
}

const std::string &Server::endpoint(void) const noexcept
{
  return m_endpoint;
}

void Server::rememberCalls(std::chrono::milliseconds window,
                           std::size_t maxCalls,
                           std::size_t maxBytes)
{
  if (m_started)
  {
    throw std::logic_error(
        "zRPC::Server::rememberCalls() must be called before start().");
  }
  m_rememberFor = window;
  m_maxRemembered = maxCalls;
  m_maxRememberedBytes = maxBytes;
}

void Server::start(void)
{
  if (m_started.exchange(true))
  {
    throw std::logic_error("zRPC::Server::start() may only be called once.");
  }

  // Workers are created here rather than in the constructor so that every
  // RPC bound beforehand is visible to them (thread creation synchronises),
  // and bind() refuses to change the map from now on.
  for (uint32_t n = 0; n < m_nWorkers; ++n)
  {
    m_th.emplace_back([this]() { worker(); });
  }

  // Routing IDs of the workers waiting for a request. Requests only go to an
  // idle worker, so one slow RPC never delays another while a worker is free.
  // (A plain proxy to a DEALER hands requests out round-robin, queueing them
  // behind busy workers.)
  std::deque<zmq::message_t> idle;

  // Calls by request ID, so that a resent call runs at most once
  CallMemory calls(m_rememberFor, m_maxRemembered, m_maxRememberedBytes);

  try
  {
    while (true)
    {
      zmq::pollitem_t items[] = {
          {m_brokerBackend.handle(), 0, ZMQ_POLLIN, 0},
          {m_brokerFrontend.handle(), 0, ZMQ_POLLIN, 0},
      };

      // While every worker is busy, leave client requests queued in 0MQ
      detail::retryInterrupted(
          [&]()
          {
            return zmq::poll(items, idle.empty() ? 1U : 2U,
                             std::chrono::milliseconds{-1});
          });
      const auto now = std::chrono::steady_clock::now();
      calls.expire(now);

      if (items[0].revents & ZMQ_POLLIN)
      {
        // From a worker: [worker][client][reply] or [worker][control]
        zmq::message_t worker;
        zmq::message_t frame;
        (void)m_brokerBackend.recv(worker);
        (void)m_brokerBackend.recv(frame);

        if (frame.more())
        {
          // Send the reply to every connection waiting for this call (the
          // client plus any resends that arrived meanwhile), or just to the
          // client if the call had no ID. The worker is free again.
          zmq::message_t reply;
          (void)m_brokerBackend.recv(reply);
          std::vector<zmq::message_t> waiting;
          if (const auto peeked = detail::peek(reply.data(), reply.size()))
          {
            waiting = calls.finish(peeked->id, reply, now);
          }
          if (waiting.empty())
          {
            waiting.push_back(std::move(frame));
          }
          for (auto &client : waiting)
          {
            (void)m_brokerFrontend.send(client, zmq::send_flags::sndmore);
            (void)m_brokerFrontend.send(
                zmq::const_buffer(reply.data(), reply.size()),
                zmq::send_flags::none);
          }
          idle.push_back(std::move(worker));
        }
        else if (frame.to_string_view() == kStop)
        {
          // A bound RPC called stop() and its reply has just been forwarded
          shutdown();
        }
        else
        {
          idle.push_back(std::move(worker));
        }
      }

      if (items[1].revents & ZMQ_POLLIN)
      {
        // From a client: [client][request]. A request is a single frame;
        // discard any extra frames so they cannot be taken for the next one.
        zmq::message_t client;
        zmq::message_t request;
        (void)m_brokerFrontend.recv(client);
        (void)m_brokerFrontend.recv(request);
        for (bool more = request.more(); more;)
        {
          zmq::message_t extra;
          (void)m_brokerFrontend.recv(extra);
          more = extra.more();
        }

        const auto peeked = detail::peek(request.data(), request.size());
        switch (peeked ? calls.arrive(*peeked, client) :
                         CallMemory::Action::Run)
        {
          case CallMemory::Action::Run:
            if (!dispatch(m_brokerBackend, idle, client, request))
            {
              // Every idle worker turned out to have stopped (each logged
              // why). The call never ran, so it is not remembered.
              if (peeked)
              {
                calls.abandon(peeked->id);
              }
              const auto error = errorReply(ErrorCode::Unknown,
                                            "Server has no running workers",
                                            requestId(request));
              (void)m_brokerFrontend.send(client, zmq::send_flags::sndmore);
              (void)m_brokerFrontend.send(
                  zmq::const_buffer(error.data(), error.size()),
                  zmq::send_flags::none);
            }
            break;

          case CallMemory::Action::Wait:
            // The running call's reply goes to this connection too
            break;

          case CallMemory::Action::Replay:
          {
            const auto &stored = calls.reply(peeked->id);
            (void)m_brokerFrontend.send(client, zmq::send_flags::sndmore);
            (void)m_brokerFrontend.send(
                zmq::const_buffer(stored.data(), stored.size()),
                zmq::send_flags::none);
            break;
          }

          case CallMemory::Action::Unavailable:
          {
            const auto error = errorReply(
                ErrorCode::ReplyUnavailable,
                "Call " + peeked->id.toString() +
                    " already ran, but its reply was not kept for resends "
                    "(see Server::rememberCalls())",
                peeked->id);
            (void)m_brokerFrontend.send(client, zmq::send_flags::sndmore);
            (void)m_brokerFrontend.send(
                zmq::const_buffer(error.data(), error.size()),
                zmq::send_flags::none);
            break;
          }

          case CallMemory::Action::Conflict:
          {
            const auto error =
                errorReply(ErrorCode::BadRequest,
                           "Request ID " + peeked->id.toString() +
                               " was already used for a different call",
                           peeked->id);
            (void)m_brokerFrontend.send(client, zmq::send_flags::sndmore);
            (void)m_brokerFrontend.send(
                zmq::const_buffer(error.data(), error.size()),
                zmq::send_flags::none);
            break;
          }
        }
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    // ETERM means stop() shut the context down, which is a normal exit
    if (e.num() != ETERM)
    {
      detail::log(LogLevel::Error,
                  "Server broker for " + m_endpoint + " stopped: 0MQ error " +
                      std::to_string(e.num()) + ": " + e.what());
    }
  }
}

void Server::stop(void)
{
  // Clear the running flag. When a bound RPC calls this, the broker shuts the
  // context down once it has forwarded that RPC's reply; otherwise do it now.
  // Final cleanup will take place in the destructor.
  m_running = false;
  if (t_worker == this)
  {
    t_stopAfterReply = true;
  }
  else
  {
    shutdown();
  }
}

void Server::shutdown(void)
{
  // Replies already handed to the frontend socket are still delivered: the
  // socket lingers until they are sent when it is closed in the destructor.
  m_ctx.shutdown();
}

void Server::worker(void)
{
  t_worker = this;

  try
  {
    zmq::socket_t sock(m_ctx, zmq::socket_type::dealer);
    sock.set(zmq::sockopt::linger, detail::zmqTimeout(detail::kLinger));
    sock.connect("inproc://backend");

    // Tell the broker this worker is ready for its first request
    (void)sock.send(zmq::str_buffer(kReady), zmq::send_flags::none);

    while (m_running)
    {
      zmq::message_t identity;
      zmq::message_t msg;
      (void)detail::retryInterrupted([&]() { return sock.recv(identity); });
      (void)detail::retryInterrupted([&]() { return sock.recv(msg); });

      // Every request gets exactly one reply. A malformed request or an
      // exception thrown by a bound function is reported back to the client
      // rather than escaping this thread and terminating the whole server.
      msgpack::sbuffer res;
      try
      {
        res = process(msg);
      }
      catch (const zmq::error_t &)
      {
        throw;
      }
      catch (const std::exception &e)
      {
        res = errorReply(ErrorCode::Unknown, e.what(), requestId(msg));
      }
      catch (...)
      {
        res =
            errorReply(ErrorCode::Unknown, "Unknown exception", requestId(msg));
      }
      // A larger reply would be dropped by the client's socket, and every
      // resend would replay it, so the client would only ever see a timeout
      if (!detail::fitsMessageLimit(res))
      {
        res = errorReply(ErrorCode::Failed, detail::tooLarge("Reply", res),
                         requestId(msg));
      }
      reply(sock, identity, res);

      if (t_stopAfterReply)
      {
        // A bound RPC called stop(). This follows the reply down the same
        // pipe, so the broker only shuts down after forwarding the reply.
        (void)sock.send(zmq::str_buffer(kStop), zmq::send_flags::none);
        break;
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    // ETERM means stop() shut the context down, which is a normal exit
    if (e.num() != ETERM)
    {
      detail::log(LogLevel::Error,
                  "Server worker for " + m_endpoint + " stopped: 0MQ error " +
                      std::to_string(e.num()) + ": " + e.what());
    }
  }
}

msgpack::sbuffer Server::process(const zmq::message_t &msg)
{
  // Validate the envelope (version, CRC, size limits) and unpack the request
  detail::Opened request;
  try
  {
    request = detail::open(msg.data(), msg.size());
  }
  catch (const ProtocolError &e)
  {
    // Echo the ID if the request's CRC holds (e.g. a malformed payload), so
    // the client can match the error to its call. A corrupted request has no
    // trustworthy ID; its client resends.
    return errorReply(ErrorCode::BadRequest, e.what(), requestId(msg));
  }
  const auto &id = request.id;

  // Unpack and convert RPC name and arguments
  std::tuple<std::string, msgpack::object> rpc;
  try
  {
    request.body.get().convert(rpc);
  }
  catch (const msgpack::type_error &)
  {
    return errorReply(ErrorCode::BadRequest,
                      "Malformed request: expected [name, [arguments...]]", id);
  }

  auto &&name = std::get<0>(rpc);
  auto &&args = std::get<1>(rpc);

  // Call the RPC
  auto rpcIt = m_rpcs.find(name);
  if (rpcIt == m_rpcs.end())
  {
    return errorReply(ErrorCode::NotFound, "'" + name + "' RPC not found!", id);
  }

  try
  {
    auto result = rpcIt->second(args);
    return detail::seal(detail::Status::Ok, result->get(), id);
  }
  catch (const detail::InvalidArguments &e)
  {
    return errorReply(ErrorCode::InvalidArguments, e.what(), id);
  }
  catch (const std::exception &e)
  {
    return errorReply(ErrorCode::Failed, e.what(), id);
  }
  catch (...)
  {
    return errorReply(ErrorCode::Failed, "Unknown exception", id);
  }
}

void Server::reply(zmq::socket_t &sock,
                   zmq::message_t &identity,
                   const msgpack::sbuffer &res) const
{
  // Reply with the identity of the message for the broker
  zmq::message_t copied_id;
  copied_id.copy(identity);
  (void)sock.send(copied_id, zmq::send_flags::sndmore);
  (void)sock.send(zmq::const_buffer(res.data(), res.size()),
                  zmq::send_flags::none);
}

}  // namespace zRPC
