/*
 * @file   client.cpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 1:51:26 pm
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
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include "zRPC.hpp"

class Message
{
public:
  int v{-1};

  MSGPACK_DEFINE(v)
};

void f1(zRPC::zRPCClient &client)
{
  try
  {
    client.call("f1");
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void f2(zRPC::zRPCClient &client)
{
  try
  {
    Message m;
    m.v = 2;
    client.call("f2", m);
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void f3(zRPC::zRPCClient &client)
{
  try
  {
    auto res_f3 = client.call("f3");
    Message mr;
    std::cout << "f3 pre-result = " << mr.v << std::endl;
    std::cout << "f3 result = " << res_f3.get().as<Message>().v << std::endl;
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void l1(zRPC::zRPCClient &client)
{
  try
  {
    for (int i = 0; i < 10; i++)
    {
      auto res = client.call("l1", 7, 3 + i);
      std::cout << "l1 result = " << res.get().as<double>() << std::endl;
    }
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void l2(zRPC::zRPCClient &client)
{
  try
  {
    for (int i = 0; i < 10; i++)
    {
      auto res = client.call("l2", 11, 9 + i);
      std::cout << "l2 result = " << res.get().as<int>() << std::endl;
    }
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void f4(zRPC::zRPCClient &client)
{
  try
  {
    Message m;
    m.v = 1;
    auto res = client.call("f4", m);
    std::cout << "f4 result = " << res.get().as<Message>().v << std::endl;
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

int main()
{
  int major = -1;
  int minor = -1;
  int patch = -1;
  zmq_version(&major, &minor, &patch);
  printf("Installed ZeroMQ version: %d.%d.%d\n", major, minor, patch);

  zRPC::zRPCClient client("TEST-CLIENT", "tcp://localhost", 12345);

  auto f3t = std::thread(f3, std::ref(client));
  auto f2t = std::thread(f2, std::ref(client));
  auto f1t = std::thread(f1, std::ref(client));

  auto l1t = std::thread(l1, std::ref(client));
  auto l2t = std::thread(l2, std::ref(client));

  auto f4t = std::thread(f4, std::ref(client));

  f1t.join();
  f2t.join();
  f3t.join();

  l1t.join();
  l2t.join();

  f4t.join();

  return 0;
}