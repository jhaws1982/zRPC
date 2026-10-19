/*
 * @file   test_pubsub.cpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Tests of zRPC::Publisher and zRPC::Subscriber
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

#include <atomic>
#include <ios>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include "helpers.hpp"

using namespace zrpc_test;

namespace
{
/**
 * @brief Topics and values a subscription callback has seen
 */
template <class T>
class Received
{
public:
  zRPC::Subscriber::cb_type<T> callback()
  {
    return [this](const std::string &topic, T &data)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_topics.insert(topic);
      m_values.push_back(data);
    };
  }

  std::size_t count()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_values.size();
  }

  std::set<std::string> topics()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_topics;
  }

  std::vector<T> values()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_values;
  }

private:
  std::mutex m_mutex;
  std::set<std::string> m_topics;
  std::vector<T> m_values;
};

/**
 * @brief Keep publishing until a subscription has received enough messages.
 * Subscriptions take a moment to reach the publisher, and 0MQ drops
 * publications that nobody is subscribed to yet.
 */
template <class T, class R>
bool publishUntil(zRPC::Publisher &pub,
                  const std::string &topic,
                  const T &data,
                  R &received,
                  std::size_t wanted = 1)
{
  return eventually(
      [&]()
      {
        pub.publish(topic, data);
        return received.count() >= wanted;
      });
}
/**
 * @brief A bare 0MQ publisher that can send anything at all
 */
class RawPublisher
{
public:
  RawPublisher() : m_ctx(1), m_sock(m_ctx, zmq::socket_type::pub)
  {
    m_sock.bind("tcp://127.0.0.1:*");
    m_endpoint = m_sock.get(zmq::sockopt::last_endpoint);
  }

  const std::string &endpoint() const
  {
    return m_endpoint;
  }

  void send(const std::string &topic, const std::string &data)
  {
    (void)m_sock.send(zmq::buffer(topic), zmq::send_flags::sndmore);
    (void)m_sock.send(zmq::buffer(data), zmq::send_flags::none);
  }

  /**
   * @brief A correctly sealed publication of an int
   */
  static std::string sealed(const std::string &topic, int v)
  {
    return bytes(zRPC::detail::seal(zRPC::detail::Status::Ok,
                                    std::make_tuple(topic, v)));
  }

private:
  zmq::context_t m_ctx;
  zmq::socket_t m_sock;
  std::string m_endpoint;
};

/**
 * @brief Installs a log handler that records messages, and restores the
 * previous handler on destruction. Declare it before any Subscriber so the
 * subscription threads are gone before it is.
 */
class CapturedLog
{
public:
  CapturedLog() :
      m_previous(zRPC::setLogHandler(
          [this](zRPC::LogLevel level, const std::string &message)
          {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.emplace_back(level, message);
          }))
  {
  }

  ~CapturedLog()
  {
    zRPC::setLogHandler(std::move(m_previous));
  }

  bool contains(zRPC::LogLevel level, const std::string &text)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto &[l, message] : m_entries)
    {
      if (l == level && message.find(text) != std::string::npos)
      {
        return true;
      }
    }
    return false;
  }

private:
  std::mutex m_mutex;
  std::vector<std::pair<zRPC::LogLevel, std::string>> m_entries;
  zRPC::LogHandler m_previous;
};

/**
 * @brief Subscribe to "t" on a raw publisher, send a malformed message and
 * then a marker until the marker arrives
 *
 * @return bool Whether the marker arrived
 */
bool sendMalformedThenMarker(RawPublisher &raw, std::atomic<int> &markers)
{
  return eventually(
      [&]()
      {
        raw.send("t", "\xc1 not msgpack");
        raw.send("t", RawPublisher::sealed("t", 7));
        return markers > 0;
      });
}
}  // namespace

TEST(PubSub, DeliversTypedData)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<Msg> got;
  zRPC::Subscriber sub;
  sub.subscribe<Msg>(pub.endpoint(), "msg", got.callback());

  ASSERT_TRUE(publishUntil(pub, "msg", Msg{9, "payload"}, got));
  EXPECT_EQ(got.values().front(), (Msg{9, "payload"}));
  EXPECT_EQ(got.topics(), std::set<std::string>{"msg"});
}

TEST(PubSub, DeliversLargeArrays)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<std::vector<int>> got;
  zRPC::Subscriber sub;
  sub.subscribe<std::vector<int>>(pub.endpoint(), "v", got.callback());

  const std::vector<int> samples(200000, 3);
  ASSERT_TRUE(publishUntil(pub, "v", samples, got));
  EXPECT_EQ(got.values().front(), samples);
}

TEST(PubSub, TopicsMatchExactlyNotByPrefix)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<int> a;
  zRPC::Subscriber sub;
  sub.subscribe<int>(pub.endpoint(), "A", a.callback());

  ASSERT_TRUE(eventually(
      [&]()
      {
        pub.publish("ABC", 2);
        pub.publish("Alpha", 3);
        pub.publish("A", 1);
        return a.count() >= 3;
      }));
  EXPECT_EQ(a.topics(), std::set<std::string>{"A"});
  for (int v : a.values())
  {
    EXPECT_EQ(v, 1);
  }
}

TEST(PubSub, TopicsMayContainColons)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<double> got;
  zRPC::Subscriber sub;
  sub.subscribe<double>(pub.endpoint(), "sensor:temp:1", got.callback());

  ASSERT_TRUE(publishUntil(pub, "sensor:temp:1", 21.5, got));
  EXPECT_DOUBLE_EQ(got.values().front(), 21.5);
}

TEST(PubSub, EmptyTopicReceivesEveryTopic)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<int> all;
  zRPC::Subscriber sub;
  sub.subscribe<int>(pub.endpoint(), "", all.callback());

  ASSERT_TRUE(eventually(
      [&]()
      {
        pub.publish("x", 1);
        pub.publish("y", 2);
        return all.topics().size() == 2;
      }));
}

TEST(PubSub, BadMessagesAndThrowingCallbacksDoNotEndTheSubscription)
{
  // A raw publisher can send anything at all
  zmq::context_t ctx(1);
  zmq::socket_t raw(ctx, zmq::socket_type::pub);
  raw.bind("tcp://127.0.0.1:*");
  const auto endpoint = raw.get(zmq::sockopt::last_endpoint);

  // Declared before the subscriber, whose thread uses them until it is gone
  std::atomic<int> calls{0};
  std::atomic<int> good{0};
  zRPC::Subscriber sub;
  sub.subscribe<int>(endpoint, "t",
                     [&](const std::string &, int &v)
                     {
                       ++calls;
                       if (v == 1)
                       {
                         throw std::runtime_error("callback failed");
                       }
                       ++good;
                     });

  auto send = [&](const std::string &topic, const std::string &data)
  {
    (void)raw.send(zmq::buffer(topic), zmq::send_flags::sndmore);
    (void)raw.send(zmq::buffer(data), zmq::send_flags::none);
  };
  auto sealed = [](const std::string &topic, int v)
  {
    return bytes(zRPC::detail::seal(zRPC::detail::Status::Ok,
                                    std::make_tuple(topic, v)));
  };

  auto corrupt = sealed("t", 5);
  corrupt[corrupt.size() - 6] =
      static_cast<char>(corrupt[corrupt.size() - 6] ^ 0x01);

  ASSERT_TRUE(eventually(
      [&]()
      {
        (void)raw.send(zmq::str_buffer("t"), zmq::send_flags::none);
        send("t", "\xc1 not msgpack");  // 0xc1 is never valid msgpack
        send("t", corrupt);             // CRC mismatch
        send("t", sealed("u", 5));      // sealed topic differs from frame
        send("t", sealed("t", 1));      // callback throws
        send("t", sealed("t", 2));      // must still be delivered
        return good > 0;
      }));
  EXPECT_GT(calls.load(), good.load());

  // Reporting a bad message must not leave std::cerr printing in hex
  EXPECT_EQ(std::cerr.flags() & std::ios::basefield, std::ios::dec);
}

TEST(PubSub, PublishFromManyThreads)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<int> got;
  zRPC::Subscriber sub;
  sub.subscribe<int>(pub.endpoint(), "n", got.callback());
  ASSERT_TRUE(publishUntil(pub, "n", -1, got));

  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
  {
    threads.emplace_back(
        [&pub, t]()
        {
          for (int i = 0; i < 200; ++i)
          {
            pub.publish("n", t * 1000 + i);
          }
        });
  }
  for (auto &th : threads)
  {
    th.join();
  }
  EXPECT_TRUE(eventually([&]() { return got.count() > 1; }));
}

TEST(PubSub, PublishAcceptsTemporariesAndConstData)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  Received<std::string> got;
  zRPC::Subscriber sub;
  sub.subscribe<std::string>(pub.endpoint(), "s", got.callback());

  const std::string constant = "const";
  ASSERT_TRUE(eventually(
      [&]()
      {
        pub.publish("s", std::string("temporary"));
        pub.publish("s", constant);
        return got.count() >= 2;
      }));
}

TEST(PubSub, OversizedPublicationIsRejected)
{
  zRPC::Publisher pub("tcp://127.0.0.1:*");
  const auto limit = static_cast<std::size_t>(zRPC::detail::kMaxMessageSize);
  EXPECT_THROW(pub.publish("t", std::string(limit, 'x')), zRPC::ProtocolError);
  EXPECT_NO_THROW(pub.publish("t", std::string(1000, 'x')));
}

TEST(PubSub, DestroyingAPublisherDoesNotWaitForAStalledSubscriber)
{
  // A subscriber that stays connected but never reads, so publications
  // back up in the publisher's queue
  zmq::context_t ctx(1);
  zmq::socket_t stalled(ctx, zmq::socket_type::sub);
  stalled.set(zmq::sockopt::linger, 0);
  stalled.set(zmq::sockopt::rcvhwm, 1);

  auto pub = std::make_unique<zRPC::Publisher>("tcp://127.0.0.1:*");
  stalled.connect(pub->endpoint());
  stalled.set(zmq::sockopt::subscribe, "");
  std::this_thread::sleep_for(200ms);

  const std::string chunk(1U << 20, 'x');
  for (int i = 0; i < 64; ++i)
  {
    pub->publish("t", chunk);
  }

  const auto start = Clock::now();
  pub.reset();
  EXPECT_LT(msSince(start), 5000);
}

TEST(PubSub, PortConstructorAndBindFailure)
{
  zRPC::Publisher pub(uint16_t{0});
  EXPECT_NE(pub.endpoint().find("tcp://0.0.0.0:"), std::string::npos);
  EXPECT_THROW(zRPC::Publisher{connectable(pub.endpoint())}, zmq::error_t);
  EXPECT_THROW(zRPC::Publisher{"not-a-uri"}, zmq::error_t);
}

TEST(Logging, SubscriptionProblemsGoToTheLogHandler)
{
  CapturedLog log;
  RawPublisher raw;
  std::atomic<int> good{0};
  zRPC::Subscriber sub;
  sub.subscribe<int>(raw.endpoint(), "t",
                     [&](const std::string &, int &v)
                     {
                       if (v == 1)
                       {
                         throw std::runtime_error("callback failed");
                       }
                       ++good;
                     });

  EXPECT_TRUE(eventually(
      [&]()
      {
        raw.send("t", "\xc1 not msgpack");
        raw.send("t", RawPublisher::sealed("t", 1));
        raw.send("t", RawPublisher::sealed("t", 2));
        return good > 0 &&
               log.contains(zRPC::LogLevel::Warning,
                            "dropped a malformed message") &&
               log.contains(zRPC::LogLevel::Error, "threw: callback failed");
      }));
}

TEST(Logging, DefaultHandlerWritesToStderr)
{
  std::ostringstream captured;
  auto *stderrBuf = std::cerr.rdbuf(captured.rdbuf());
  {
    RawPublisher raw;
    std::atomic<int> markers{0};
    zRPC::Subscriber sub;
    sub.subscribe<int>(raw.endpoint(), "t",
                       [&](const std::string &, int &) { ++markers; });
    EXPECT_TRUE(sendMalformedThenMarker(raw, markers));
  }
  std::cerr.rdbuf(stderrBuf);

  EXPECT_NE(captured.str().find("zRPC warning: Subscription to 't'"),
            std::string::npos)
      << captured.str();
}

TEST(Logging, EmptyHandlerDiscardsMessages)
{
  auto previous = zRPC::setLogHandler({});
  std::ostringstream captured;
  auto *stderrBuf = std::cerr.rdbuf(captured.rdbuf());
  {
    RawPublisher raw;
    std::atomic<int> markers{0};
    zRPC::Subscriber sub;
    sub.subscribe<int>(raw.endpoint(), "t",
                       [&](const std::string &, int &) { ++markers; });
    EXPECT_TRUE(sendMalformedThenMarker(raw, markers));
  }
  std::cerr.rdbuf(stderrBuf);
  zRPC::setLogHandler(std::move(previous));

  EXPECT_EQ(captured.str(), "");
}
