/*
 * @file   zRPCPublisher.inl
 * @author Jonathan Haws
 * @date   01-Apr-2022 9:34:13 am
 *
 * @brief 0MQ-based publisher with MessagePack support
 *
 * @copyright Jonathan Haws -- 2022
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

template <class T>
void Publisher::publish(const std::string &topic, T &data)
{
  try
  {
    if (m_pub)
    {
      // Pack the data into a stringstream and calculate CRC
      std::stringstream cbuf;
      msgpack::pack(cbuf, data);
      std::uint32_t crc =
          CRC::Calculate(cbuf.str().data(), cbuf.str().size(), m_crcTable);

      // Create a tuple with the topic name, packed data, and CRC
      auto data_tuple = std::make_tuple(topic, cbuf.str(), crc);

      // Pack the new tuple into an object and send to the server
      auto sbuf = std::make_shared<msgpack::sbuffer>();
      msgpack::pack(*sbuf, data_tuple);
      const auto sndbuf = topic + ":" + std::string(sbuf->data(), sbuf->size());
      (void)m_pub.send(zmq::const_buffer(sndbuf.c_str(), sndbuf.length()));
    }
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Error " << e.num() << ": " << e.what() << std::endl;
  }
}

}  // namespace zRPC