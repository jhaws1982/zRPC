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
msgpack::object_handle Client::call(const std::string &name, A... args)
{
  try
  {
    // Ensure socket is connected to the server
    zmq::socket_t l_sock(m_ctx, ZMQ_DEALER);
    l_sock.set(zmq::sockopt::routing_id, m_idBase + std::to_string(m_idx++));
    l_sock.connect(m_uri);

    if (l_sock)
    {
      // Create a tuple with the RPC name and arguments
      auto args_tuple = std::make_tuple(args...);
      auto call_tuple = std::make_tuple(name, args_tuple);

      // Pack the tuple into a stringstream and calculate CRC
      std::stringstream cbuf;
      msgpack::pack(cbuf, call_tuple);
      std::uint32_t crc =
          CRC::Calculate(cbuf.str().data(), cbuf.str().size(), m_crcTable);
      auto crc_tuple = std::make_tuple(cbuf.str(), crc);

      // Pack the new tuple into an object and send to the server
      auto sbuf = std::make_shared<msgpack::sbuffer>();
      msgpack::pack(*sbuf, crc_tuple);
      (void)l_sock.send(zmq::const_buffer(sbuf->data(), sbuf->size()));

      // Wait for response
      zmq::message_t msg;
      auto rxres = l_sock.recv(msg);
      if (rxres && (rxres.value() > 0))
      {
        auto obj = msgpack::unpack(static_cast<char *>(msg.data()), msg.size());
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