/*
 * @file   zRPCClient.inl
 * @author Jonathan Haws
 * @date   21-Dec-2021 8:44:46 pm
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

namespace zRPC
{
template <typename... A>
msgpack::object_handle Client::call(const std::string &name, A... args)
{
  return call(m_timeout, name, args...);
}

template <typename... A>
msgpack::object_handle Client::call(std::chrono::milliseconds timeout,
                                    const std::string &name,
                                    A... args)
{
  // Ensure socket is connected to the server
  zmq::socket_t l_sock(m_ctx, zmq::socket_type::dealer);
  l_sock.set(zmq::sockopt::rcvtimeo, detail::zmqTimeout(timeout));
  // Once the call returns nothing on this socket is worth delivering; an
  // unsent request after a timeout must not hold up closing the context
  l_sock.set(zmq::sockopt::linger, 0);
  l_sock.set(zmq::sockopt::maxmsgsize, detail::kMaxMessageSize);
  l_sock.connect(m_uri);

  // Send [name, [args...]] in a CRC-protected envelope
  const auto request = detail::seal(
      detail::Status::Ok,
      std::forward_as_tuple(name, std::forward_as_tuple(args...)));
  (void)l_sock.send(zmq::const_buffer(request.data(), request.size()),
                    zmq::send_flags::none);

  // Wait for response or timeout event
  zmq::message_t msg;
  if (!l_sock.recv(msg))
  {
    throw TimeoutError("zRPC client '" + m_identity + "': no reply to '" +
                       name + "' within " + std::to_string(timeout.count()) +
                       " ms");
  }

  auto reply = detail::open(msg.data(), msg.size());
  if (reply.status == detail::Status::Error)
  {
    throw RemoteError(reply.body.get().as<Error>());
  }
  return std::move(reply.body);
}
}  // namespace zRPC
