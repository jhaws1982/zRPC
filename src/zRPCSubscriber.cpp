/*
 * @file   zRPCSubscriber.inl
 * @author Jonathan Haws
 * @date   01-Apr-2022 10:31:18 am
 *
 * @brief 0MQ-based subscriber with MessagePack support
 *
 * @copyright Jonathan Haws -- 2022
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

#include "zRPC.hpp"

using namespace zRPC;

Subscriber::Subscriber() : m_ctx(1), m_crcTable(CRC::CRC_32())
{
}

Subscriber::~Subscriber()
{
  // Clear the running flag and shutdown the context
  m_running = false;
  m_ctx.shutdown();

  for (auto &th : m_handlers)
  {
    if (th.joinable())
    {
      th.join();
    }
  }
}