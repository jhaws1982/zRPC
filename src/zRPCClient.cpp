/*
 * @file   zRPCClient.cpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 3:11:07 pm
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

#include <random>
#include "zRPC.hpp"

namespace zRPC
{

Client::Client(const std::string &identity,
               const std::string &uri,
               std::chrono::milliseconds timeout) :
    m_ctx(1),
    m_identity(identity),
    m_uri(uri),
    m_timeout(timeout),
    m_idPrefix(0)
{
  // Mix a hardware random value with the time and this object's address, in
  // case std::random_device is deterministic on some platform
  std::random_device rd;
  m_idPrefix =
      (std::uint64_t{rd()} << 32) ^ std::uint64_t{rd()} ^
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this));
}

RequestId Client::newRequestId(void)
{
  const std::uint64_t halves[2] = {m_idPrefix, m_nextCall.fetch_add(1)};
  RequestId id;
  std::memcpy(id.bytes.data(), halves, sizeof(halves));
  return id;
}

}  // namespace zRPC
