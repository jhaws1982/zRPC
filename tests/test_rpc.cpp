/*
 * @file   test_rpc.cpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Tests of zRPC::Server and zRPC::Client
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
#include <future>
#include <mutex>
#include <set>
#include <type_traits>
#include "helpers.hpp"

using namespace zrpc_test;
using zRPC::ErrorCode;

namespace
{
int add(int a, int b)
{
  return a + b;
}

int addNoexcept(int a, int b) noexcept
{
  return a + b;
}

void noop()
{
}

Msg echo(const Msg &m)
{
  return m;
}

Msg &bump(Msg &m)
{
  ++m.v;
  return m;
}

std::vector<std::string> looksLikeAnError()
{
  // Shaped exactly like a zRPC::Error used to be on the wire
  return {"boom"};
}

int boom()
{
  throw std::runtime_error("handler exploded");
}

int sleepFor(int ms)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  return ms;
}

/**
 * @brief Function object whose count lives in the object itself, guarded as
 * Server::bind() requires because calls may run on any worker
 */
struct Counter
{
  int n{0};

  int operator()()
  {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    return ++n;
  }
};

struct Calculator
{
  int base{100};

  int add(int a)
  {
    return base + a;
  }

  int get() const
  {
    return base;
  }

  int addNoexcept(int a) noexcept
  {
    return base + a + 1;
  }
};

/**
 * @brief The error code a remote error carries, or nothing if the call
 * succeeded or failed some other way
 */
template <typename F>
std::optional<ErrorCode> remoteError(F &&call)
{
  try
  {
    call();
  }
  catch (const zRPC::RemoteError &e)
  {
    return e.code();
  }
  return std::nullopt;
}

/**
 * @brief A stand-in server that answers every request with 42, optionally
 * more than once, and records the connection (routing ID) each request
 * arrived on
 */
class CountingServer
{
public:
  explicit CountingServer(int repliesPerRequest = 1) :
      m_repliesPerRequest(repliesPerRequest),
      m_ctx(1),
      m_sock(m_ctx, zmq::socket_type::router)
  {
    m_sock.bind("tcp://127.0.0.1:*");
    m_sock.set(zmq::sockopt::rcvtimeo, 50);
    m_endpoint = m_sock.get(zmq::sockopt::last_endpoint);
    m_thread = std::thread([this]() { serve(); });
  }

  ~CountingServer()
  {
    m_stop = true;
    m_thread.join();
  }

  const std::string &endpoint() const
  {
    return m_endpoint;
  }

  std::size_t connections()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connections.size();
  }

  std::size_t requests()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_requests;
  }

private:
  void serve()
  {
    while (!m_stop)
    {
      zmq::message_t client;
      zmq::message_t request;
      if (!m_sock.recv(client))
      {
        continue;
      }
      (void)m_sock.recv(request);
      const auto opened = zRPC::detail::open(request.data(), request.size());
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connections.insert(client.to_string());
        ++m_requests;
      }

      const auto reply =
          zRPC::detail::seal(zRPC::detail::Status::Ok, 42, opened.id);
      for (int i = 0; i < m_repliesPerRequest; ++i)
      {
        zmq::message_t to;
        to.copy(client);
        (void)m_sock.send(to, zmq::send_flags::sndmore);
        (void)m_sock.send(zmq::const_buffer(reply.data(), reply.size()),
                          zmq::send_flags::none);
      }
    }
  }

  int m_repliesPerRequest;
  zmq::context_t m_ctx;
  zmq::socket_t m_sock;
  std::string m_endpoint;
  std::atomic<bool> m_stop{false};
  std::mutex m_mutex;
  std::set<std::string> m_connections;
  std::size_t m_requests{0};
  std::thread m_thread;
};

/**
 * @brief Error carried by a raw reply envelope
 */
zRPC::Error errorIn(const zmq::message_t &reply)
{
  auto opened = zRPC::detail::open(reply.data(), reply.size());
  EXPECT_EQ(opened.status, zRPC::detail::Status::Error);
  return opened.body.get().as<zRPC::Error>();
}
}  // namespace

static_assert(!std::is_copy_constructible_v<zRPC::Server>);
static_assert(!std::is_copy_constructible_v<zRPC::Client>);
static_assert(!std::is_copy_constructible_v<zRPC::Publisher>);
static_assert(!std::is_copy_constructible_v<zRPC::Subscriber>);
static_assert(!std::is_move_constructible_v<zRPC::Server>);

// Timeouts are durations. An int timeout no longer compiles, so call(0, "x")
// cannot be mistaken for an RPC named by a null pointer.
template <typename C>
concept TakesIntTimeout = requires(C &c) { c.call(100, std::string("x")); };
template <typename C>
concept TakesZeroAndName = requires(C &c) { c.call(0, "x"); };
template <typename C>
concept TakesDurationTimeout =
    requires(C &c) { c.call(std::chrono::milliseconds(100), "x"); };
static_assert(!TakesIntTimeout<zRPC::Client>);
static_assert(!TakesZeroAndName<zRPC::Client>);
static_assert(TakesDurationTimeout<zRPC::Client>);

class Rpc : public ::testing::Test
{
protected:
  void SetUp() override
  {
    m_srv.bind("add", &add);
    m_srv.bind("addNoexcept", &addNoexcept);
    m_srv.bind("noop", &noop);
    m_srv.bind("echo", &echo);
    m_srv.bind("bump", &bump);
    m_srv.bind("looksLikeAnError", &looksLikeAnError);
    m_srv.bind("boom", &boom);
    m_srv.bind("sleep", &sleepFor);
    m_srv.bind("blob", [](std::size_t n) { return std::string(n, 'x'); });
    m_srv.bind("samples",
               [](std::size_t n) { return std::vector<double>(n, 0.5); });
    m_srv.bind("sum",
               [](const std::vector<double> &v)
               {
                 double total = 0;
                 for (double x : v)
                 {
                   total += x;
                 }
                 return total;
               });
    m_srv.bind("mul", [](double a, double b) { return a * b; });
    m_srv.bind("greet", [](const std::string &who) { return "hello " + who; });
    m_srv.bind("count", Counter{});
    m_srv.bind("twiceNoexcept", [](int a) noexcept { return 2 * a; });
    m_srv.bind("calcAdd", &Calculator::add, &m_calc);
    m_srv.bind("calcGet", &Calculator::get, &m_calc);
    m_srv.bind("calcGetConst", &Calculator::get, &m_constCalc);
    m_srv.bind("calcAddNoexcept", &Calculator::addNoexcept, &m_calc);
    m_run = std::make_unique<RunningServer>(m_srv);
  }

  void TearDown() override
  {
    m_run.reset();
  }

  Calculator m_calc;
  const Calculator m_constCalc{7};
  zRPC::Server m_srv{"tcp://127.0.0.1:*", 4};
  std::unique_ptr<RunningServer> m_run;
  zRPC::Client m_client{"test", m_srv.endpoint(), 5000ms};
};

// ---------------------------------------------------------------------------
// Calls and binding
// ---------------------------------------------------------------------------

TEST_F(Rpc, ReturnsValuesOfEveryKindOfBoundFunction)
{
  EXPECT_EQ(m_client.call("add", 2, 3).get().as<int>(), 5);
  EXPECT_EQ(m_client.call("addNoexcept", 2, 3).get().as<int>(), 5);
  EXPECT_DOUBLE_EQ(m_client.call("mul", 1.5, 4.0).get().as<double>(), 6.0);
  EXPECT_EQ(m_client.call("greet", std::string("zRPC")).get().as<std::string>(),
            "hello zRPC");
  EXPECT_EQ(m_client.call("echo", Msg{3, "x"}).get().as<Msg>(), (Msg{3, "x"}));
  EXPECT_EQ(m_client.call("bump", Msg{3, "x"}).get().as<Msg>(), (Msg{4, "x"}));
  EXPECT_EQ(m_client.call("twiceNoexcept", 21).get().as<int>(), 42);
  EXPECT_EQ(m_client.call("count").get().as<int>(), 1);
  EXPECT_EQ(m_client.call("count").get().as<int>(), 2);
}

TEST_F(Rpc, BindsMemberFunctionsOfAnObject)
{
  EXPECT_EQ(m_client.call("calcAdd", 5).get().as<int>(), 105);
  EXPECT_EQ(m_client.call("calcGet").get().as<int>(), 100);
  EXPECT_EQ(m_client.call("calcGetConst").get().as<int>(), 7);
  EXPECT_EQ(m_client.call("calcAddNoexcept", 5).get().as<int>(), 106);
}

TEST_F(Rpc, LargeArraysTravelBothWays)
{
  // 200,000 samples is about 1.8 MB, far below the message size limit
  const auto samples = m_client.call("samples", std::size_t{200000})
                           .get()
                           .as<std::vector<double>>();
  ASSERT_EQ(samples.size(), 200000U);
  EXPECT_DOUBLE_EQ(m_client.call("sum", samples).get().as<double>(), 100000.0);
}

TEST_F(Rpc, VoidFunctionReturnsNil)
{
  EXPECT_EQ(m_client.call("noop").get().type, msgpack::type::NIL);
}

TEST_F(Rpc, ValueShapedLikeAnErrorIsStillAValue)
{
  auto res = m_client.call("looksLikeAnError");
  EXPECT_EQ(res.get().as<std::vector<std::string>>(),
            std::vector<std::string>{"boom"});
}

TEST_F(Rpc, DuplicateBindThrows)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 1);
  srv.bind("f", &noop);
  EXPECT_THROW(srv.bind("f", &noop), std::runtime_error);
}

TEST_F(Rpc, BindAfterStartAndSecondStartThrow)
{
  EXPECT_TRUE(eventually(
      [&]() { return m_client.call("add", 1, 1).get().as<int>() == 2; }));
  EXPECT_THROW(m_srv.bind("late", &noop), std::logic_error);
  EXPECT_THROW(m_srv.start(), std::logic_error);
}

// ---------------------------------------------------------------------------
// Errors are exceptions with a code, and never take the server down
// ---------------------------------------------------------------------------

TEST_F(Rpc, UnknownRpcIsNotFound)
{
  EXPECT_EQ(remoteError([&]() { m_client.call("missing"); }),
            ErrorCode::NotFound);
}

TEST_F(Rpc, WrongArgumentsAreInvalidArguments)
{
  EXPECT_EQ(remoteError([&]() { m_client.call("add", 1); }),
            ErrorCode::InvalidArguments);
  EXPECT_EQ(remoteError([&]() { m_client.call("add", 1, 2, 3); }),
            ErrorCode::InvalidArguments);
  EXPECT_EQ(remoteError([&]() { m_client.call("add", 1, std::string("x")); }),
            ErrorCode::InvalidArguments);
  EXPECT_EQ(m_client.call("add", 1, 2).get().as<int>(), 3);
}

TEST_F(Rpc, ThrowingFunctionIsFailedAndServerKeepsServing)
{
  try
  {
    m_client.call("boom");
    FAIL() << "expected RemoteError";
  }
  catch (const zRPC::RemoteError &e)
  {
    EXPECT_EQ(e.code(), ErrorCode::Failed);
    EXPECT_STREQ(e.what(), "handler exploded");
  }
  EXPECT_EQ(m_client.call("add", 1, 2).get().as<int>(), 3);
}

TEST_F(Rpc, AcceptsAnyDurationAsTimeout)
{
  EXPECT_EQ(m_client.call(2s, "add", 1, 2).get().as<int>(), 3);
  EXPECT_EQ(m_client.call(zRPC::kWaitForever, "add", 1, 2).get().as<int>(), 3);
}

TEST_F(Rpc, TimeoutThrowsTimeoutError)
{
  const auto start = Clock::now();
  EXPECT_THROW(m_client.call(100ms, "sleep", 1000), zRPC::TimeoutError);
  EXPECT_LT(msSince(start), 900);
}

TEST(RpcClient, NoServerTimesOutAndDestroysPromptly)
{
  // Find a port nobody is listening on
  std::string endpoint;
  {
    zRPC::Server probe("tcp://127.0.0.1:*", 1);
    endpoint = probe.endpoint();
  }

  const auto start = Clock::now();
  {
    zRPC::Client client("lonely", endpoint, 200ms);
    EXPECT_THROW(client.call("anything"), zRPC::TimeoutError);
  }
  EXPECT_LT(msSince(start), 1500);
}

TEST_F(Rpc, HostileRequestsGetBadRequestRepliesAndServerSurvives)
{
  const auto &ep = m_srv.endpoint();

  auto garbage = rawRequest(ep, {"hello, not msgpack"});
  ASSERT_TRUE(garbage);
  EXPECT_EQ(errorIn(*garbage).m_code, ErrorCode::BadRequest);

  auto flipped = request("add", 1, 2);
  flipped[flipped.size() - 6] =
      static_cast<char>(flipped[flipped.size() - 6] ^ 0x10);
  auto corrupt = rawRequest(ep, {flipped});
  ASSERT_TRUE(corrupt);
  EXPECT_EQ(errorIn(*corrupt).m_code, ErrorCode::BadRequest);

  msgpack::sbuffer bomb;
  msgpack::packer<msgpack::sbuffer> pk(bomb);
  pk.pack_array(0xFFFFFFFFU);
  auto bombed = rawRequest(
      ep, {bytes(zRPC::detail::sealPayload(zRPC::detail::Status::Ok, bomb))});
  ASSERT_TRUE(bombed);
  EXPECT_EQ(errorIn(*bombed).m_code, ErrorCode::BadRequest);

  msgpack::sbuffer legacy;
  msgpack::pack(legacy, std::make_tuple(std::string("add"), 42U));
  auto old = rawRequest(ep, {bytes(legacy)});
  ASSERT_TRUE(old);
  EXPECT_EQ(errorIn(*old).m_code, ErrorCode::BadRequest);

  auto notArray = rawRequest(
      ep, {bytes(zRPC::detail::seal(zRPC::detail::Status::Ok,
                                    std::make_tuple(std::string("add"), 2)))});
  ASSERT_TRUE(notArray);
  EXPECT_EQ(errorIn(*notArray).m_code, ErrorCode::InvalidArguments);

  // Extra frames are discarded rather than taken for the next request
  auto multi = rawRequest(ep, {"garbage", request("add", 1, 2)});
  ASSERT_TRUE(multi);
  EXPECT_EQ(errorIn(*multi).m_code, ErrorCode::BadRequest);

  EXPECT_EQ(m_client.call("add", 20, 22).get().as<int>(), 42);
}

TEST_F(Rpc, OversizedReplyIsReportedRatherThanTimingOut)
{
  // The client's socket would drop a reply this large, and every resend
  // would replay it: the call could only ever time out
  const auto limit = static_cast<std::size_t>(zRPC::detail::kMaxMessageSize);
  const auto start = Clock::now();
  EXPECT_EQ(remoteError([&]() { m_client.call("blob", limit); }),
            ErrorCode::Failed);
  EXPECT_LT(msSince(start), 4000);
  EXPECT_EQ(m_client.call("add", 1, 2).get().as<int>(), 3);
}

TEST_F(Rpc, OversizedMessageIsDroppedAndServerSurvives)
{
  const auto tooBig =
      static_cast<std::size_t>(zRPC::detail::kMaxMessageSize) + 1024;
  EXPECT_FALSE(rawRequest(m_srv.endpoint(), {std::string(tooBig, 'x')}, 500));
  EXPECT_EQ(m_client.call("add", 1, 2).get().as<int>(), 3);
}

// ---------------------------------------------------------------------------
// Construction and shutdown
// ---------------------------------------------------------------------------

TEST(RpcServer, PortConstructorListensOnAllInterfaces)
{
  zRPC::Server srv(uint16_t{0}, 1);
  srv.bind("add", &add);
  RunningServer run(srv);
  zRPC::Client client("port", connectable(srv.endpoint()), 5000ms);
  EXPECT_EQ(client.call("add", 1, 2).get().as<int>(), 3);
}

TEST(RpcServer, BindFailureThrows)
{
  zRPC::Server first("tcp://127.0.0.1:*", 1);
  EXPECT_THROW(zRPC::Server(first.endpoint(), 1), zmq::error_t);
  EXPECT_THROW(zRPC::Server("not-a-uri", 1), zmq::error_t);
}

TEST(RpcServer, BuiltInTerminateNoLongerExists)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 1);
  RunningServer run(srv);
  zRPC::Client client("c", srv.endpoint(), 5000ms);
  EXPECT_EQ(remoteError([&]() { client.call("terminate"); }),
            ErrorCode::NotFound);
}

TEST(RpcServer, StopFromABoundFunctionRepliesFirst)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 2);
  srv.bind("terminate", [&srv]() { srv.stop(); });
  auto stopped = std::async(std::launch::async, [&srv]() { srv.start(); });

  zRPC::Client client("c", srv.endpoint(), 5000ms);
  EXPECT_EQ(client.call("terminate").get().type, msgpack::type::NIL);
  EXPECT_EQ(stopped.wait_for(5s), std::future_status::ready);
}

TEST(RpcServer, ExternalStopEndsStart)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 2);
  auto stopped = std::async(std::launch::async, [&srv]() { srv.start(); });
  std::this_thread::sleep_for(100ms);
  srv.stop();
  EXPECT_EQ(stopped.wait_for(5s), std::future_status::ready);
}

TEST(RpcServer, StopBeforeStart)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 2);
  srv.stop();
  auto stopped = std::async(std::launch::async, [&srv]() { srv.start(); });
  EXPECT_EQ(stopped.wait_for(5s), std::future_status::ready);
}

// ---------------------------------------------------------------------------
// Concurrency and load balancing
// ---------------------------------------------------------------------------

TEST_F(Rpc, OneClientFromManyThreads)
{
  std::atomic<int> correct{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t)
  {
    threads.emplace_back(
        [&, t]()
        {
          for (int i = 0; i < 20; ++i)
          {
            if (m_client.call("add", t, i).get().as<int>() == t + i)
            {
              ++correct;
            }
          }
        });
  }
  for (auto &th : threads)
  {
    th.join();
  }
  EXPECT_EQ(correct.load(), 8 * 20);
}

TEST_F(Rpc, ClientsSharingAnIdentityDoNotCollide)
{
  // Same identity, as two processes started with the same name would use
  zRPC::Client a("same", m_srv.endpoint(), 5000ms);
  zRPC::Client b("same", m_srv.endpoint(), 5000ms);

  auto slow = std::async(std::launch::async, [&a]()
                         { return a.call("sleep", 500).get().as<int>(); });
  std::this_thread::sleep_for(100ms);
  const auto start = Clock::now();
  EXPECT_EQ(b.call("add", 1, 2).get().as<int>(), 3);
  EXPECT_LT(msSince(start), 300);
  EXPECT_EQ(slow.get(), 500);
}

TEST(RpcLoadBalancing, FastCallIsNotQueuedBehindASlowOne)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 2);
  srv.bind("sleep", &sleepFor);
  RunningServer run(srv);
  zRPC::Client client("c", srv.endpoint(), 5000ms);

  // With round-robin dealing, every second request queued behind the slow one
  auto slow = std::async(std::launch::async,
                         [&]() { return client.call("sleep", 1000); });
  std::this_thread::sleep_for(100ms);
  for (int i = 0; i < 4; ++i)
  {
    const auto start = Clock::now();
    EXPECT_EQ(client.call("sleep", 0).get().as<int>(), 0);
    EXPECT_LT(msSince(start), 400) << "call " << i;
  }
  slow.get();
}

TEST(RpcLoadBalancing, AllWorkersAreUsedAndExtraCallsWaitForAFreeWorker)
{
  constexpr int kWorkers = 4;
  constexpr int kSleepMs = 500;
  zRPC::Server srv("tcp://127.0.0.1:*", kWorkers);
  srv.bind("sleep", &sleepFor);
  RunningServer run(srv);
  zRPC::Client client("c", srv.endpoint(), 10000ms);

  auto timeCalls = [&](int calls)
  {
    const auto start = Clock::now();
    std::vector<std::future<msgpack::object_handle>> pending;
    for (int i = 0; i < calls; ++i)
    {
      pending.push_back(std::async(std::launch::async, [&]()
                                   { return client.call("sleep", kSleepMs); }));
    }
    for (auto &p : pending)
    {
      EXPECT_EQ(p.get().get().as<int>(), kSleepMs);
    }
    return msSince(start);
  };

  // One call per worker runs in parallel: about one sleep in total
  const auto oneRound = timeCalls(kWorkers);
  EXPECT_LT(oneRound, kSleepMs * 2 - 100);

  // Twice as many calls: the second half waits for free workers
  const auto twoRounds = timeCalls(2 * kWorkers);
  EXPECT_GE(twoRounds, kSleepMs * 2 - 50);
  EXPECT_LT(twoRounds, kSleepMs * 3);
}

// ---------------------------------------------------------------------------
// Connection reuse
// ---------------------------------------------------------------------------

TEST(RpcConnections, SequentialCallsReuseOneConnection)
{
  CountingServer srv;
  zRPC::Client client("c", srv.endpoint(), 5s);
  for (int i = 0; i < 20; ++i)
  {
    EXPECT_EQ(client.call("anything", i).get().as<int>(), 42);
  }
  EXPECT_EQ(srv.requests(), 20U);
  EXPECT_EQ(srv.connections(), 1U);
}

TEST(RpcConnections, ConcurrentCallsUseAtMostOneConnectionEach)
{
  CountingServer srv;
  zRPC::Client client("c", srv.endpoint(), 5s);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t)
  {
    threads.emplace_back(
        [&client]()
        {
          for (int i = 0; i < 10; ++i)
          {
            EXPECT_EQ(client.call("anything").get().as<int>(), 42);
          }
        });
  }
  for (auto &th : threads)
  {
    th.join();
  }
  EXPECT_EQ(srv.requests(), 40U);
  EXPECT_LE(srv.connections(), 4U);
}

TEST(RpcConnections, StaleRepliesOnAReusedConnectionAreSkipped)
{
  // Every reply arrives twice; the duplicate is still queued on the reused
  // connection when the next call starts, and must not be taken for its reply
  CountingServer srv(2);
  zRPC::Client client("c", srv.endpoint(), 5s);
  for (int i = 0; i < 5; ++i)
  {
    EXPECT_EQ(client.call("anything").get().as<int>(), 42);
  }
  EXPECT_EQ(srv.requests(), 5U);
  EXPECT_EQ(srv.connections(), 1U);
}

TEST_F(Rpc, CallsAfterATimeoutGetTheirOwnReplies)
{
  EXPECT_THROW(m_client.call(100ms, "sleep", 400), zRPC::TimeoutError);
  for (int i = 0; i < 3; ++i)
  {
    EXPECT_EQ(m_client.call("add", i, 1).get().as<int>(), i + 1);
  }
  // By now the timed-out call's reply has been sent too
  std::this_thread::sleep_for(400ms);
  EXPECT_EQ(m_client.call("add", 20, 22).get().as<int>(), 42);
}

// ---------------------------------------------------------------------------
// At-most-once calls
// ---------------------------------------------------------------------------

namespace
{
/**
 * @brief A server whose "work" RPC counts how often it really runs
 */
class CountingRuns
{
public:
  explicit CountingRuns(std::chrono::milliseconds window = 5min,
                        std::size_t maxCalls = 100000,
                        const std::string &uri = "tcp://127.0.0.1:*") :
      m_srv(uri, 2)
  {
    m_srv.rememberCalls(window, maxCalls);
    // Counts a run when it starts, so a duplicate shows up at once
    m_srv.bind("work",
               [this](int ms)
               {
                 const int run = ++m_runs;
                 std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                 return run;
               });
    m_srv.bind("add", &add);
    m_running = std::make_unique<RunningServer>(m_srv);
  }

  int runs() const
  {
    return m_runs;
  }

  const std::string &endpoint() const
  {
    return m_srv.endpoint();
  }

private:
  std::atomic<int> m_runs{0};
  zRPC::Server m_srv;
  std::unique_ptr<RunningServer> m_running;
};

/**
 * @brief A sealed request carrying a chosen request ID
 */
template <typename... A>
std::string requestWithId(const zRPC::RequestId &id,
                          const std::string &name,
                          const A &...args)
{
  return bytes(zRPC::detail::seal(
      zRPC::detail::Status::Ok,
      std::forward_as_tuple(name, std::forward_as_tuple(args...)), id));
}

zRPC::RequestId idNumber(std::uint8_t n)
{
  zRPC::RequestId id;
  id.bytes.fill(0x5A);
  id.bytes.back() = n;
  return id;
}

/**
 * @brief Value carried by a raw reply envelope
 */
int valueIn(const zmq::message_t &reply)
{
  auto opened = zRPC::detail::open(reply.data(), reply.size());
  EXPECT_EQ(opened.status, zRPC::detail::Status::Ok);
  return opened.body.get().as<int>();
}
}  // namespace

TEST(AtMostOnce, ResendsDoNotRunACallTwice)
{
  CountingRuns srv;
  zRPC::Client client("c", srv.endpoint(), 5s);
  client.setResendInterval(50ms);

  // About eight resends arrive while "work" runs, with a worker free to run
  // each one if the server let it
  EXPECT_EQ(client.call("work", 400).get().as<int>(), 1);
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(srv.runs(), 1);
}

TEST(AtMostOnce, RetryAfterATimeoutGetsTheOriginalResult)
{
  CountingRuns srv;
  zRPC::Client client("c", srv.endpoint(), 5s);

  try
  {
    client.call(100ms, "work", 400);
    FAIL() << "expected TimeoutError";
  }
  catch (const zRPC::TimeoutError &e)
  {
    // Still running: the retry waits for that run
    EXPECT_EQ(client.call(e.requestId(), 5s, "work", 400).get().as<int>(), 1);
    // Finished: the retry gets the stored reply
    EXPECT_EQ(client.call(e.requestId(), 5s, "work", 400).get().as<int>(), 1);
  }
  EXPECT_EQ(srv.runs(), 1);
}

TEST(AtMostOnce, ReusingAnIdForADifferentCallIsRejected)
{
  CountingRuns srv;
  zRPC::Client client("c", srv.endpoint(), 5s);

  try
  {
    client.call(50ms, "work", 200);
    FAIL() << "expected TimeoutError";
  }
  catch (const zRPC::TimeoutError &e)
  {
    EXPECT_EQ(
        remoteError([&]() { client.call(e.requestId(), 5s, "work", 300); }),
        ErrorCode::BadRequest);
    EXPECT_EQ(
        remoteError([&]() { client.call(e.requestId(), 5s, "add", 1, 2); }),
        ErrorCode::BadRequest);
  }
  EXPECT_TRUE(eventually([&]() { return srv.runs() == 1; }));
}

TEST(AtMostOnce, ACorruptedCopyCannotClaimTheId)
{
  CountingRuns srv;
  const auto id = idNumber(1);
  auto corrupt = requestWithId(id, "work", 0);
  corrupt[corrupt.size() - 6] =
      static_cast<char>(corrupt[corrupt.size() - 6] ^ 0x01);

  auto bad = rawRequest(srv.endpoint(), {corrupt});
  ASSERT_TRUE(bad);
  EXPECT_EQ(errorIn(*bad).m_code, ErrorCode::BadRequest);

  auto good = rawRequest(srv.endpoint(), {requestWithId(id, "work", 0)});
  ASSERT_TRUE(good);
  EXPECT_EQ(valueIn(*good), 1);
  EXPECT_EQ(srv.runs(), 1);
}

TEST(AtMostOnce, AZeroWindowForgetsFinishedCalls)
{
  CountingRuns srv(0ms);
  const auto id = idNumber(1);
  ASSERT_TRUE(rawRequest(srv.endpoint(), {requestWithId(id, "work", 0)}));
  auto again = rawRequest(srv.endpoint(), {requestWithId(id, "work", 0)});
  ASSERT_TRUE(again);
  EXPECT_EQ(valueIn(*again), 2);
  EXPECT_EQ(srv.runs(), 2);
}

TEST(AtMostOnce, TheOldestCallsAreForgottenBeyondTheLimit)
{
  CountingRuns srv(5min, 2);
  for (std::uint8_t n = 1; n <= 3; ++n)
  {
    auto reply =
        rawRequest(srv.endpoint(), {requestWithId(idNumber(n), "work", 0)});
    ASSERT_TRUE(reply);
    EXPECT_EQ(valueIn(*reply), n);
  }

  // Still remembered: the stored reply, no new run
  auto third =
      rawRequest(srv.endpoint(), {requestWithId(idNumber(3), "work", 0)});
  ASSERT_TRUE(third);
  EXPECT_EQ(valueIn(*third), 3);
  EXPECT_EQ(srv.runs(), 3);

  // Forgotten to stay within two remembered calls: it runs again
  auto first =
      rawRequest(srv.endpoint(), {requestWithId(idNumber(1), "work", 0)});
  ASSERT_TRUE(first);
  EXPECT_EQ(valueIn(*first), 4);
}

TEST(AtMostOnce, RememberCallsAfterStartThrows)
{
  zRPC::Server srv("tcp://127.0.0.1:*", 1);
  RunningServer run(srv);
  EXPECT_TRUE(eventually(
      [&]()
      {
        try
        {
          srv.rememberCalls(1min);
          return false;
        }
        catch (const std::logic_error &)
        {
          return true;
        }
      }));
}

TEST(AtMostOnce, ResendsRecoverACallLostWithItsServer)
{
  // The first server takes the request and stops before replying; only a
  // resend can get the call to the server that replaces it
  auto srv = std::make_unique<CountingRuns>();
  const auto endpoint = srv->endpoint();
  zRPC::Client client("c", endpoint, 10s);
  client.setResendInterval(200ms);

  auto pending = std::async(std::launch::async,
                            [&]() { return client.call("work", 300); });
  ASSERT_TRUE(eventually([&]() { return srv->runs() == 1; }));
  srv.reset();
  srv = std::make_unique<CountingRuns>(5min, 100000, endpoint);

  // The replacement remembers nothing, so it runs the call again
  EXPECT_EQ(pending.get().get().as<int>(), 1);
  EXPECT_EQ(srv->runs(), 1);
}

TEST(AtMostOnce, ClientRecoversWhenTheServerRestartsBetweenCalls)
{
  auto srv = std::make_unique<CountingRuns>();
  const auto endpoint = srv->endpoint();
  zRPC::Client client("c", endpoint, 10s);
  client.setResendInterval(200ms);
  EXPECT_EQ(client.call("add", 1, 2).get().as<int>(), 3);

  // The call starts while the server is down; resends reach the new one
  srv.reset();
  auto pending = std::async(std::launch::async,
                            [&]() { return client.call("add", 20, 22); });
  std::this_thread::sleep_for(500ms);
  srv = std::make_unique<CountingRuns>(5min, 100000, endpoint);
  EXPECT_EQ(pending.get().get().as<int>(), 42);
}

TEST(RpcConnections, OversizedRequestIsRejectedBeforeSending)
{
  CountingServer srv;
  zRPC::Client client("c", srv.endpoint(), 5s);
  const auto limit = static_cast<std::size_t>(zRPC::detail::kMaxMessageSize);
  const auto start = Clock::now();
  EXPECT_THROW(client.call("anything", std::string(limit, 'x')),
               zRPC::ProtocolError);
  EXPECT_LT(msSince(start), 1000);
  EXPECT_EQ(srv.requests(), 0U);
}
