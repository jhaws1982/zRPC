/*
 * @file   unit.cpp
 * @author Jonathan Haws
 * @date   23-Dec-2021 9:10:52 pm
 *
 * @brief [REPLACE WITH DESCRIPTIVE TEXT]
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
#include <thread>
#include "zRPC.hpp"

void l1(zRPC::Client &client)
{
  try
  {
    for (int i = 0; i < 5; i++)
    {
      auto res = client.call("l1", 2, 2 * i - 1);
      std::cout << "l1 result = " << res.get().as<int>() << std::endl;
      assert(res.get().as<int>() == (2 + (2 * i - 1)));
    }
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void l2(zRPC::Client &client)
{
  try
  {
    for (int i = 0; i < 5; i++)
    {
      auto res = client.call("l2", 2, i);
      std::cout << "l2 result = " << res.get().as<double>() << std::endl;
      assert(res.get().as<double>() == (2 * i));
    }
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

void client(void)
{
  std::cout << "Starting zRPC client!" << std::endl;
  zRPC::Client client("TEST-CLIENT", "tcp://localhost:12345");

  auto l1t = std::thread(l1, std::ref(client));
  auto l2t = std::thread(l2, std::ref(client));
  l1t.join();
  l2t.join();

  auto res = client.call("l3");
  try
  {
    std::cout << "l3 result = " << res.get().as<double>() << std::endl;
  }
  catch (const msgpack::v1::type_error &e)
  {
    std::cout << "l3 error = '" << res.get().as<zRPC::Error>().m_msg << "'"
              << std::endl;
    assert(res.get().as<zRPC::Error>().m_msg == "'l3' RPC not found!");
  }

  client.call("terminate");  // shutdown the server

  std::cout << " EXITING CLIENT THREAD!" << std::endl;
}

void server(void)
{
  std::cout << "Starting zRPC server!" << std::endl;
  zRPC::Server srv(12345, 4);

  srv.bind("l1",
           [](int a, int b)
           {
             sleep(1);
             return a + b;
           });
  srv.bind("l2",
           [](double a, double b)
           {
             sleep(2);
             return a * b;
           });
  srv.start();

  std::cout << " EXITING SERVER THREAD!" << std::endl;
}

void pub(void)
{
  using namespace std::chrono_literals;

  //  Prepare our context and socket
  zRPC::Publisher publisher(54321);
  std::cout << "Starting zRPC publisher!" << std::endl;

  std::string msg;
  for (int i = 0; i < 12; ++i)
  {
    msg = "This is message A.";
    publisher.publish("A", msg);

    msg = "This is message B.";
    publisher.publish("B", msg);

    std::this_thread::sleep_for(1s);
  }
}

void sub(void)
{
  using namespace std::chrono_literals;

  //  Prepare our context and socket
  zRPC::Subscriber subscriber;
  std::cout << "Starting zRPC subscriber!" << std::endl;

  subscriber.subscribe<std::string>(
      "tcp://localhost:54321", "A",
      [](const std::string &topic, const std::string &data) -> void
      {
        std::cout << topic << ": " << data << std::endl;
        assert(data == "This is message A.");
      });

  subscriber.subscribe<std::string>(
      "tcp://localhost:54321", "B",
      [](const std::string &topic, const std::string &data) -> void
      {
        std::cout << topic << ": " << data << std::endl;
        assert(data == "This is message B.");
      });

  for (int i = 0; i < 10; ++i)
  {
    std::this_thread::sleep_for(1s);
  }
}

int main(void)
{
  int major = -1;
  int minor = -1;
  int patch = -1;
  zmq_version(&major, &minor, &patch);
  printf(" ** Installed ZeroMQ version: %d.%d.%d\n", major, minor, patch);

  // Client/Server test
  auto cl = std::thread(client);
  auto srv = std::thread(server);

  cl.join();
  srv.join();

  // Pub/Sub test
  auto pth = std::thread(pub);
  auto sth = std::thread(sub);

  pth.join();
  sth.join();

  return 0;
}