/*
 * @file   zRPCClient.cpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 3:11:07 pm
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

#include <algorithm>
#include <random>
#include "zRPC.hpp"

namespace zRPC
{

Client::Client(const std::string &identity,
               const std::string &uri,
               std::chrono::milliseconds timeout) :
    m_ctx(1),
    m_identity(identity),
    m_uri(uri),
    m_timeout(timeout),
    m_idPrefix(0)
{
  // Mix a hardware random value with the time and this object's address, in
  // case std::random_device is deterministic on some platform
  std::random_device rd;
  m_idPrefix =
      (std::uint64_t{rd()} << 32) ^ std::uint64_t{rd()} ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));
}

zmq::socket_t Client::acquire(void)
{
  {
    std::lock_guard<std::mutex> lock(m_poolMutex);
    if (!m_idle.empty())
    {
      auto sock = std::move(m_idle.back());
      m_idle.pop_back();
      return sock;
    }
  }

  zmq::socket_t sock(m_ctx, zmq::socket_type::dealer);
  // An unsent request is not worth delivering once its call has given up,
  // and must not hold up closing the context
  sock.set(zmq::sockopt::linger, 0);
  sock.set(zmq::sockopt::maxmsgsize, detail::kMaxMessageSize);
  sock.connect(m_uri);
  return sock;
}

void Client::release(zmq::socket_t sock)
{
  std::lock_guard<std::mutex> lock(m_poolMutex);
  m_idle.push_back(std::move(sock));
}

msgpack::object_handle Client::exchange(const RequestId &id,
                                        const msgpack::sbuffer &request,
                                        const std::string &name,
                                        std::chrono::milliseconds timeout)
{
  using std::chrono::milliseconds;
  using Clock = std::chrono::steady_clock;

  // The server would drop the connection rather than receive this, and the
  // call would only ever time out
  if (!detail::fitsMessageLimit(request))
  {
    throw ProtocolError("zRPC client '" + m_identity + "': " +
                        detail::tooLarge("Request to '" + name + "'", request));
  }

  // Negative timeouts, and any too long to represent, never expire
  const auto deadline = detail::addSaturated(Clock::now(), timeout);
  const milliseconds resend{m_resendMs.load()};

  // A 0MQ error destroys the socket with this scope instead of returning it
  auto sock = acquire();
  auto nextSend = Clock::now();
  while (true)
  {
    const auto now = Clock::now();
    if (now >= deadline)
    {
      // The reply may still arrive on this socket, so it is not reused
      throw TimeoutError("zRPC client '" + m_identity + "': no reply to '" +
                             name + "' within " +
                             std::to_string(timeout.count()) + " ms",
                         id);
    }

    if (now >= nextSend)
    {
      // Every send carries the same request ID, so however many reach the
      // server, it runs the call at most once
      (void)sock.send(zmq::const_buffer(request.data(), request.size()),
                      zmq::send_flags::none);
      nextSend = resend > milliseconds::zero() ?
                     detail::addSaturated(now, resend) :
                     Clock::time_point::max();
    }

    // Wait for the reply until the next resend or the deadline. A wait too
    // long for 0MQ's int milliseconds is cut short, and the loop waits again.
    const auto until = std::min(nextSend, deadline);
    int wait = -1;
    if (until != Clock::time_point::max())
    {
      wait = static_cast<int>(std::clamp<long long>(
          std::chrono::ceil<milliseconds>(until - now).count(), 0, INT_MAX));
    }
    sock.set(zmq::sockopt::rcvtimeo, wait);

    zmq::message_t msg;
    if (!detail::retryInterrupted([&]() { return sock.recv(msg); }))
    {
      continue;
    }

    detail::Opened reply;
    try
    {
      reply = detail::open(msg.data(), msg.size());
    }
    catch (const ProtocolError &)
    {
      release(std::move(sock));
      throw;
    }

    // A reply to an earlier call that used this socket: not ours
    if (reply.id != id)
    {
      continue;
    }

    release(std::move(sock));
    if (reply.status == detail::Status::Error)
    {
      throw RemoteError(reply.body.get().as<Error>());
    }
    return std::move(reply.body);
  }
}

void Client::setResendInterval(std::chrono::milliseconds interval)
{
  m_resendMs = interval.count();
}

RequestId Client::newRequestId(void)
{
  const std::uint64_t halves[2] = {m_idPrefix, m_nextCall.fetch_add(1)};
  RequestId id;
  std::memcpy(id.bytes.data(), halves, sizeof(halves));
  return id;
}

}  // namespace zRPC
