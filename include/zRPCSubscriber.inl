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

#include <iostream>
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
  std::lock_guard<std::mutex> lock(m_handlersMutex);
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
    sock.set(zmq::sockopt::maxmsgsize, detail::kMaxMessageSize);
    sock.connect(uri);

    // Setup the subscription to the specific topic
    sock.set(zmq::sockopt::subscribe, topic);

    while (m_running)
    {
      // A publication is two frames: [topic][data]
      zmq::message_t rtopicMsg;
      zmq::message_t msg;
      (void)sock.recv(rtopicMsg, zmq::recv_flags::none);
      if (!rtopicMsg.more())
      {
        continue;
      }
      (void)sock.recv(msg, zmq::recv_flags::none);
      for (bool more = msg.more(); more;)
      {
        zmq::message_t extra;
        (void)sock.recv(extra, zmq::recv_flags::none);
        more = extra.more();
      }

      // 0MQ subscriptions match on prefix, so "A" also receives "ABC"; only
      // an empty topic means every topic
      if (!topic.empty() && rtopicMsg.to_string_view() != topic)
      {
        continue;
      }

      try
      {
        // Validate the envelope (version, CRC, size limits) and unpack the
        // sealed [topic, data] pair
        auto pub = detail::open(msg.data(), msg.size());
        std::tuple<std::string, msgpack::object> pubdata;
        pub.body.get().convert(pubdata);

        auto &&rtopic = std::get<0>(pubdata);
        if (rtopic != rtopicMsg.to_string_view())
        {
          throw ProtocolError("topic frame '" +
                              std::string(rtopicMsg.to_string_view()) +
                              "' does not match the publication");
        }

        // Unpack the data to published data type
        T d;
        std::get<1>(pubdata).convert(d);
        cb(rtopic, d);
      }
      catch (const std::exception &e)
      {
        // A message that cannot be decoded, or a callback that throws, must
        // not end the subscription or, by escaping this thread, the process
        std::cerr << " !! Subscriber Error on topic '" << topic
                  << "': " << e.what() << std::endl;
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    // ETERM means the Subscriber is being destroyed, which is a normal exit
    if (e.num() != ETERM)
    {
      std::cerr << " !! ZMQ Subscriber Error " << e.num() << ": " << e.what()
                << std::endl;
    }
  }
}

}  // namespace zRPC
