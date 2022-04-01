/*
 * @file   zRPCSubscriber.inl
 * @author Jonathan Haws
 * @date   01-Apr-2022 10:31:18 am
 *
 * @brief 0MQ-based subscriber with MessagePack support
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

#include "zRPC.hpp"

using namespace zRPC;

namespace zRPC
{

template <class T>
void Subscriber::subscribe(const std::string &uri,
                           const std::string &topic,
                           cb_type<T> cb)
{
  m_running = true;
  m_handlers.emplace_back(
      std::thread([this, uri, topic, cb]() { handler<T>(uri, topic, cb); }));
}

template <class T>
void Subscriber::handler(const std::string &uri,
                         const std::string &topic,
                         cb_type<T> cb)
{
  try
  {
    zmq::socket_t sock(m_ctx, zmq::socket_type::sub);
    sock.connect(uri);

    // Setup the subscription to the specific topic
    sock.set(zmq::sockopt::subscribe, topic);

    while (m_running)
    {
      zmq::message_t msg;
      (void)sock.recv(msg, zmq::recv_flags::none);

      try
      {
        // Trim the topic from the beginning of the message for proper unpacking
        std::string message(static_cast<char *>(msg.data()), msg.size());
        message.erase(0, message.find_first_of(':') + 1);

        // Unpack the message to the topic/data/CRC tuple
        auto data_tuple = msgpack::unpack(message.data(), message.length());
        std::tuple<std::string, std::string, std::uint32_t> pubdata;
        data_tuple.get().convert(pubdata);

        auto &&rtopic = std::get<0>(pubdata);
        auto &&rdata = std::get<1>(pubdata);
        auto &&rcrc = std::get<2>(pubdata);
        std::uint32_t check =
            CRC::Calculate(rdata.data(), rdata.size(), m_crcTable);

        if (check == rcrc)
        {
          // Unpack the data to published data type
          T d;
          auto data_obj =
              msgpack::unpack(static_cast<char *>(rdata.data()), rdata.size());
          data_obj.get().convert(d);
          cb(rtopic, d);
        }
        else
        {
          std::cerr << std::hex << "Bad checksum: CRC=" << rcrc
                    << " != " << check << "=Check" << std::endl;
        }
      }
      catch (const msgpack::v1::type_error &e)
      {
        std::cerr << " !! MessagePack Type Error: " << e.what() << std::endl;
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Worker Error " << e.num() << ": " << e.what()
              << std::endl;
  }
}

}  // namespace zRPC
