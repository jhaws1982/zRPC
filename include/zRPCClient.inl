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

#include <iostream>

namespace zRPC
{
template <typename... A>
msgpack::object_handle zRPCClient::call(const std::string &name, A... args)
{
  try
  {
    // Ensure socket is connected to the server
    zmq::socket_t m_sock(m_cxt, ZMQ_DEALER);
    m_sock.set(zmq::sockopt::routing_id, m_idBase + std::to_string(m_idx++));
    m_sock.connect(m_uri);

    if (m_sock)
    {
      // Create a tuple with the RPC name and arguments
      auto args_tuple = std::make_tuple(args...);
      auto call_tuple = std::make_tuple(name, args_tuple);

      // Pack the tuple into an object and send to the server
      auto sbuf = std::make_shared<msgpack::sbuffer>();
      msgpack::pack(*sbuf, call_tuple);
      m_sock.send(zmq::const_buffer(sbuf->data(), sbuf->size()));

      // Wait for response
      zmq::message_t zmqobj;
      auto rxres = m_sock.recv(zmqobj);
      if (rxres && (rxres.value() > 0))
      {
        auto obj =
            msgpack::unpack(static_cast<char *>(zmqobj.data()), zmqobj.size());
        return obj;
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Error " << e.num() << ": " << e.what() << std::endl;
  }
  return msgpack::object_handle();
}
}  // namespace zRPC