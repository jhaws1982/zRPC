/*
 * @file   server.cpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 2:04:18 pm
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

#include <unistd.h>
#include <iostream>
#include <string>
#include "zRPC.hpp"

class Message
{
public:
  int v{5};

  MSGPACK_DEFINE(v)
};

void showData(const std::string &topic, Message &data)
{
  std::cout << " ** Topic '" << topic << "': " << data.v << std::endl;
}

int main(void)
{
  using namespace std::chrono_literals;

  //  Prepare our context and socket
  zRPC::Subscriber subscriber;
  std::cout << "Starting zRPC subscriber!" << std::endl;

  // subscriber.subscribe<Message>(
  //     "tcp://localhost:54321", "B",
  //     [](const std::string &topic, Message &data) -> void
  //     { showData(std::string("--") + topic + std::string("--"), data); });

  // zRPC::Subscriber::cb_type<Message> cb = [](const std::string &topic,
  //                                            Message &data) -> void
  // {
  //   showData(std::string("**") + topic + std::string("**"), data);
  // };

  // subscriber.subscribe<Message>("tcp://localhost:54321", "A", cb);

  subscriber.subscribe<Message>("tcp://localhost:54321", "", &showData);

  for (int i = 0; i < 20; ++i)
  {
    std::this_thread::sleep_for(1s);
  }

  std::cout << "Exiting server!" << std::endl;
}