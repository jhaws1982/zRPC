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
