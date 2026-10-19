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

#include <iostream>

namespace zRPC
{

template <class T>
void Publisher::publish(const std::string &topic, const T &data)
{
  try
  {
    if (m_pub)
    {
      // Seal the topic along with the data so the CRC covers it too
      const auto pub = detail::seal(detail::Status::Ok,
                                    std::forward_as_tuple(topic, data));

      // Send the envelope after the topic. The topic travels in a frame of its
      // own so that subscribers can match it exactly, whatever characters it
      // contains.
      std::lock_guard<std::mutex> lock(m_mutex);
      (void)m_pub.send(zmq::buffer(topic), zmq::send_flags::sndmore);
      (void)m_pub.send(zmq::const_buffer(pub.data(), pub.size()),
                       zmq::send_flags::none);
    }
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Error " << e.num() << ": " << e.what() << std::endl;
  }
}

}  // namespace zRPC