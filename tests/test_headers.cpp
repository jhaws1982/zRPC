/*
 * @file   test_headers.cpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Checks that the public headers are self-contained and do not leak
 * names into the global namespace
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

// Each header must compile on its own and tolerate being included twice. The
// order and the repeats are the point, so keep clang-format from sorting them.
// clang-format off
#include "zRPCSupport.hpp"
#include "zRPCSupport.hpp"
#include "zRPCProtocol.hpp"
#include "zRPCProtocol.hpp"
#include "zRPC.hpp"
#include "zRPC.hpp"
// clang-format on

#include <gtest/gtest.h>
#include <type_traits>

// An application's own types may share names with zRPC's, because zRPC keeps
// its names inside namespace zRPC
struct Client
{
  int id{1};
};
struct Server
{
  int id{2};
};
struct Error
{
  int id{3};
};
struct Publisher
{
  int id{4};
};
struct Subscriber
{
  int id{5};
};

TEST(Headers, DoNotLeakNamesIntoTheGlobalNamespace)
{
  static_assert(!std::is_same_v<Client, zRPC::Client>);
  static_assert(!std::is_same_v<Error, zRPC::Error>);

  Client c;
  Server s;
  Error e;
  Publisher p;
  Subscriber sub;
  EXPECT_EQ(c.id + s.id + e.id + p.id + sub.id, 15);
}
