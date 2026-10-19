/*
 * @file   helpers.hpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Shared helpers for the zRPC unit tests
 *
 * @copyright Jonathan Haws -- 2026
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

#ifndef ZRPC_TESTS_HELPERS_HPP_
#define ZRPC_TESTS_HELPERS_HPP_

#include <gtest/gtest.h>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "zRPC.hpp"

namespace zrpc_test
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

/**
 * @brief A MessagePack-able user type
 */
struct Msg
{
  int v{0};
  std::string s;

  MSGPACK_DEFINE(v, s)

  bool operator==(const Msg &) const = default;
};

/**
 * @brief Milliseconds elapsed since a start time
 */
inline long long msSince(Clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                               start)
      .count();
}

/**
 * @brief Turn an endpoint bound to every interface into one a client can
 * connect to, e.g. "tcp://0.0.0.0:4000" -> "tcp://127.0.0.1:4000"
 */
inline std::string connectable(std::string endpoint)
{
  const std::string any = "0.0.0.0";
  const auto pos = endpoint.find(any);
  if (pos != std::string::npos)
  {
    endpoint.replace(pos, any.size(), "127.0.0.1");
  }
  return endpoint;
}

/**
 * @brief Poll until a condition holds or a timeout passes
 *
 * @return bool Whether the condition held
 */
inline bool eventually(const std::function<bool()> &condition,
                       std::chrono::milliseconds timeout = 5s)
{
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline)
  {
    if (condition())
    {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return condition();
}

/**
 * @brief Runs a server's broker on a background thread, and stops and joins
 * it on destruction
 */
class RunningServer
{
public:
  explicit RunningServer(zRPC::Server &srv) :
      m_srv(srv),
      m_thread([this]() { m_srv.start(); })
  {
  }

  ~RunningServer()
  {
    m_srv.stop();
    join();
  }

  /**
   * @brief Wait for start() to return
   */
  void join()
  {
    if (m_thread.joinable())
    {
      m_thread.join();
    }
  }

private:
  zRPC::Server &m_srv;
  std::thread m_thread;
};

/**
 * @brief Send raw frames to a server, as a misbehaving client would, and
 * return the reply if one arrives in time
 */
inline std::optional<zmq::message_t> rawRequest(
    const std::string &endpoint,
    const std::vector<std::string> &frames,
    int timeoutMs = 2000)
{
  zmq::context_t ctx(1);
  zmq::socket_t sock(ctx, zmq::socket_type::dealer);
  sock.set(zmq::sockopt::rcvtimeo, timeoutMs);
  sock.set(zmq::sockopt::linger, 0);
  sock.connect(endpoint);
  for (std::size_t i = 0; i < frames.size(); ++i)
  {
    const auto flags = (i + 1 < frames.size()) ? zmq::send_flags::sndmore :
                                                 zmq::send_flags::none;
    (void)sock.send(zmq::buffer(frames[i]), flags);
  }

  zmq::message_t reply;
  if (!sock.recv(reply))
  {
    return std::nullopt;
  }
  return reply;
}

/**
 * @brief Bytes of an envelope
 */
inline std::string bytes(const msgpack::sbuffer &buf)
{
  return std::string(buf.data(), buf.size());
}

/**
 * @brief A sealed, well-formed request for the RPC `name`
 */
template <typename... A>
std::string request(const std::string &name, const A &...args)
{
  return bytes(zRPC::detail::seal(
      zRPC::detail::Status::Ok,
      std::forward_as_tuple(name, std::forward_as_tuple(args...))));
}

}  // namespace zrpc_test

#endif  // ZRPC_TESTS_HELPERS_HPP_
