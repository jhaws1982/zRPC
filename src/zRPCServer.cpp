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

#include <csignal>
#include <deque>
#include <iostream>

using namespace zRPC;

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
 * @brief Build a response carrying a zRPC::Error with the given message
 *
 * @param[in] msg Error message for the client
 * @return std::unique_ptr<msgpack::object_handle> Response to send back
 */
std::unique_ptr<msgpack::object_handle> errorResponse(const std::string &msg)
{
  Error err;
  err.m_msg = msg;
  auto zone = std::make_unique<msgpack::zone>();
  auto rtnobj = msgpack::object(err, *zone);
  return std::make_unique<msgpack::object_handle>(rtnobj, std::move(zone));
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
    m_crcTable(CRC::CRC_32())
{
  // Start the broker sockets and bind to their ports. Peers sending larger
  // messages are disconnected by 0MQ before the data reaches a worker. A bind
  // failure (e.g. address in use) throws, before any worker thread exists.
  m_brokerFrontend.set(zmq::sockopt::maxmsgsize, detail::kMaxMessageSize);
  m_brokerFrontend.bind(uri);
  m_brokerBackend.bind("inproc://backend");

  m_running = true;
  for (uint32_t n = 0; n < nWorkers; ++n)
  {
    // Create and connect the worker sockets now
    m_th.emplace_back(std::thread([this]() { worker(); }));
  }
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

void Server::start(void)
{
  // Routing IDs of the workers waiting for a request. Requests only go to an
  // idle worker, so one slow RPC never delays another while a worker is free.
  // (A plain proxy to a DEALER hands requests out round-robin, queueing them
  // behind busy workers.)
  std::deque<zmq::message_t> idle;

  try
  {
    while (true)
    {
      zmq::pollitem_t items[] = {
          {m_brokerBackend.handle(), 0, ZMQ_POLLIN, 0},
          {m_brokerFrontend.handle(), 0, ZMQ_POLLIN, 0},
      };

      // While every worker is busy, leave client requests queued in 0MQ
      zmq::poll(items, idle.empty() ? 1U : 2U, std::chrono::milliseconds{-1});

      if (items[0].revents & ZMQ_POLLIN)
      {
        // From a worker: [worker][client][reply] or [worker][control]
        zmq::message_t worker;
        zmq::message_t frame;
        (void)m_brokerBackend.recv(worker);
        (void)m_brokerBackend.recv(frame);

        if (frame.more())
        {
          // Forward the reply to the client; the worker is free again
          zmq::message_t reply;
          (void)m_brokerBackend.recv(reply);
          (void)m_brokerFrontend.send(frame, zmq::send_flags::sndmore);
          (void)m_brokerFrontend.send(reply, zmq::send_flags::none);
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

        (void)m_brokerBackend.send(idle.front(), zmq::send_flags::sndmore);
        idle.pop_front();
        (void)m_brokerBackend.send(client, zmq::send_flags::sndmore);
        (void)m_brokerBackend.send(request, zmq::send_flags::none);
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    // ETERM means stop() shut the context down, which is a normal exit
    if (e.num() != ETERM)
    {
      std::cerr << " !! ZMQ Broker Error " << e.num() << ": " << e.what()
                << std::endl;
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
    sock.connect("inproc://backend");

    // Tell the broker this worker is ready for its first request
    (void)sock.send(zmq::str_buffer(kReady), zmq::send_flags::none);

    while (m_running)
    {
      zmq::message_t identity;
      zmq::message_t msg;
      (void)sock.recv(identity);
      (void)sock.recv(msg);

      // Every request gets exactly one reply. A malformed request or an
      // exception thrown by a bound function is reported back to the client
      // rather than escaping this thread and terminating the whole server.
      std::unique_ptr<msgpack::object_handle> res;
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
        res = errorResponse(e.what());
      }
      catch (...)
      {
        res = errorResponse("Unknown exception");
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
      std::cerr << " !! ZMQ Worker Error " << e.num() << ": " << e.what()
                << std::endl;
    }
  }
}

std::unique_ptr<msgpack::object_handle> Server::process(
    const zmq::message_t &msg)
{
  // Unpack and convert the message and CRC
  auto crcdata =
      msgpack::unpack(static_cast<const char *>(msg.data()), msg.size(),
                      nullptr, nullptr, detail::unpackLimits());
  std::tuple<std::string, std::uint32_t> crcrpc;
  crcdata.get().convert(crcrpc);

  auto &&rpcmsg = std::get<0>(crcrpc);
  auto &&crc = std::get<1>(crcrpc);
  std::uint32_t check =
      CRC::Calculate(rpcmsg.data(), rpcmsg.size(), m_crcTable);

  if (check != crc)
  {
    std::stringstream ss;
    ss << std::hex << "Bad checksum: CRC=" << crc << " != " << check
       << "=Checked";
    std::cout << ss.str() << std::endl;
    return errorResponse(ss.str());
  }

  // Unpack and convert RPC name and arguments
  std::tuple<std::string, msgpack::object> rpc;
  auto data = msgpack::unpack(rpcmsg.data(), rpcmsg.size(), nullptr, nullptr,
                              detail::unpackLimits());
  data.get().convert(rpc);

  auto &&name = std::get<0>(rpc);
  auto &&args = std::get<1>(rpc);

  // Call the RPC
  auto rpcIt = m_rpcs.find(name);
  if (rpcIt == m_rpcs.end())
  {
    return errorResponse("'" + name + "' RPC not found!");
  }
  return rpcIt->second(args);
}

void Server::reply(zmq::socket_t &sock,
                   zmq::message_t &identity,
                   std::unique_ptr<msgpack::object_handle> &res) const
{
  // Reply with the identity of the message for the broker
  zmq::message_t copied_id;
  copied_id.copy(identity);
  (void)sock.send(copied_id, zmq::send_flags::sndmore);

  // Pack the result into an object
  auto sbuf = std::make_shared<msgpack::sbuffer>();
  msgpack::pack(*sbuf, res->get());
  (void)sock.send(zmq::const_buffer(sbuf->data(), sbuf->size()),
                  zmq::send_flags::none);
}