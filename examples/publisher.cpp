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

int main(void)
{
  using namespace std::chrono_literals;

  //  Prepare our context and socket
  zRPC::Publisher publisher(54321);
  std::cout << "Starting zRPC publisher!" << std::endl;

  Message m;
  for (int i = 0; i < 30; ++i)
  {
    m.v = i;
    publisher.publish("A", m);

    m.v = i + 100;
    publisher.publish("B", m);

    std::this_thread::sleep_for(1s);
  }

  std::cout << "Exiting server!" << std::endl;
}