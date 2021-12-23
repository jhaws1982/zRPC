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

namespace TestNS
{
void fNS(Message &m)
{
  std::cout << " ** Executing " << __FUNCTION__ << ": " << m.v << std::endl;
  return;
}
}  // namespace TestNS

void f1(void)
{
  std::cout << " ** Executing " << __FUNCTION__ << std::endl;
  return;
}
void f2(const Message &m)
{
  std::cout << " ** Executing " << __FUNCTION__ << ": " << m.v << std::endl;
  return;
}
Message f3(void)
{
  std::cout << " ** Executing " << __FUNCTION__ << std::endl;
  sleep(5);
  return Message();
}
Message &f4(Message &m)
{
  m.v = 73;
  std::cout << " ** Executing " << __FUNCTION__ << ": " << m.v << std::endl;
  return m;
}

int main()
{
  //  Prepare our context and socket
  zRPC::zRPCServer srv(12345, 4);
  std::cout << "Starting zRPC server!" << std::endl;

  Message m;
  srv.bind("f1", &f1);
  srv.bind("f2", &f2);
  srv.bind("f3", &f3);
  srv.bind("f4", &f4);
  srv.bind("l1",
           [](int a, uint8_t b) -> double
           {
             std::cout << "Inside l1" << std::endl;
             sleep(7);
             return a + b;
           });
  srv.bind("l2",
           [](int &a, uint16_t b)
           {
             std::cout << "Inside l2" << std::endl;
             sleep(2);
             return a + b;
           });
  srv.bind("fNS", &TestNS::fNS);
  srv.start();

  std::cout << "Exiting!" << std::endl;
  return 0;
}