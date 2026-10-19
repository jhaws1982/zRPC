/*
 * @file   clientDetached.cpp
 * @author Seif Hadrich
 * @date   3-May-2021 10:30:00 am
 *
 * @brief demo application for running a client in detached non blocking thread
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

void f4(zRPC::Client &client, int timeout)
{
  try
  {
    Message m;
    m.v = 1;
    auto res = client.call(timeout, "f4", m);
    std::cout << "f4 result = " << res.get().as<Message>().v << std::endl;
  }
  catch (const std::exception &e)
  {
    std::cerr << e.what() << '\n';
  }
}

int main(void)
{
  zRPC::Client client("TEST-CLIENT-Detached", "tcp://localhost:12345");

  int i = 0;
  while (i < 100)
  {
    // Run f4 client with timeout enabled
    int timeout = 100;  // 100ms
    std::cout << "Call detached f4 thread number " << i << std::endl;
    std::thread(f4, std::ref(client), timeout).detach();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    i++;
  }

  client.call("terminate");  // shutdown the server
  sleep(1);
}