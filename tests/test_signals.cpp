/*
 * @file   test_signals.cpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief zRPC's threads must survive signals handled by the application
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

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include "helpers.hpp"

using namespace zrpc_test;

namespace
{
void ignoreSignal(int)
{
}
}  // namespace

TEST(Signals, ServerAndSubscriptionThreadsSurviveInterruptions)
{
  // A handler installed without SA_RESTART, as an application might for
  // reloads, makes a blocking call on whichever thread gets the signal fail
  // with EINTR
  struct sigaction action
  {
  };
  action.sa_handler = &ignoreSignal;
  sigemptyset(&action.sa_mask);
  struct sigaction previous
  {
  };
  ASSERT_EQ(sigaction(SIGUSR1, &action, &previous), 0);

  // zRPC's threads start with SIGUSR1 unblocked
  zRPC::Server srv("tcp://127.0.0.1:*", 2);
  srv.bind("add", [](int a, int b) { return a + b; });
  auto server = std::make_unique<RunningServer>(srv);

  zRPC::Publisher pub("tcp://127.0.0.1:*");
  std::atomic<int> received{0};
  auto sub = std::make_unique<zRPC::Subscriber>();
  sub->subscribe<int>(pub.endpoint(), "t",
                      [&](const std::string &, int &) { ++received; });

  zRPC::Client client("c", srv.endpoint(), 2s);
  ASSERT_EQ(client.call("add", 1, 2).get().as<int>(), 3);

  // Keep the signals off this thread, so that they land on zRPC's
  sigset_t usr1;
  sigemptyset(&usr1);
  sigaddset(&usr1, SIGUSR1);
  ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &usr1, nullptr), 0);
  for (int i = 0; i < 50; ++i)
  {
    kill(getpid(), SIGUSR1);
    std::this_thread::sleep_for(2ms);
  }

  // The broker and both workers still serve, and the subscription still
  // receives
  for (int i = 0; i < 10; ++i)
  {
    EXPECT_EQ(client.call("add", i, 1).get().as<int>(), i + 1);
  }
  EXPECT_TRUE(eventually(
      [&]()
      {
        pub.publish("t", 1);
        return received > 0;
      }));

  sub.reset();
  server.reset();
  // Unblock first, so a signal still pending reaches the no-op handler
  ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &usr1, nullptr), 0);
  ASSERT_EQ(sigaction(SIGUSR1, &previous, nullptr), 0);
}
