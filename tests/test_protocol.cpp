/*
 * @file   test_protocol.cpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Tests of the CRC-protected message envelope
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

#include <algorithm>
#include "helpers.hpp"

using namespace zrpc_test;
using zRPC::ProtocolError;
using zRPC::detail::open;
using zRPC::detail::seal;
using zRPC::detail::sealPayload;
using zRPC::detail::Status;

TEST(Crc32, MatchesTheStandardCheckValue)
{
  const char check[] = "123456789";
  EXPECT_EQ(zRPC::detail::crc32(check, 9), 0xCBF43926U);
  EXPECT_EQ(zRPC::detail::crc32(check, 0), 0U);
}

TEST(Envelope, RoundTripsValuesAndErrors)
{
  const Msg sent{7, "hello"};
  auto ok = seal(Status::Ok, sent);
  auto opened = open(ok.data(), ok.size());
  EXPECT_EQ(opened.status, Status::Ok);
  EXPECT_EQ(opened.body.get().as<Msg>(), sent);

  zRPC::Error err;
  err.m_msg = "nope";
  err.m_code = zRPC::ErrorCode::NotFound;
  auto bad = seal(Status::Error, err);
  opened = open(bad.data(), bad.size());
  EXPECT_EQ(opened.status, Status::Error);
  const auto got = opened.body.get().as<zRPC::Error>();
  EXPECT_EQ(got.m_msg, "nope");
  EXPECT_EQ(got.m_code, zRPC::ErrorCode::NotFound);
}

TEST(Envelope, BodyOutlivesTheReceivedBytes)
{
  msgpack::object_handle kept;
  {
    std::vector<char> rx;
    {
      auto buf = seal(Status::Ok, Msg{7, std::string(300, 'x')});
      rx.assign(buf.data(), buf.data() + buf.size());
    }
    kept = std::move(open(rx.data(), rx.size()).body);
    std::fill(rx.begin(), rx.end(), '\0');
  }
  EXPECT_EQ(kept.get().as<Msg>(), (Msg{7, std::string(300, 'x')}));
}

TEST(Envelope, DetectsEverySingleBitFlip)
{
  for (const auto status : {Status::Ok, Status::Error})
  {
    const auto sealed = bytes(seal(status, Msg{1, "hello"}));
    for (std::size_t i = 0; i < sealed.size(); ++i)
    {
      for (int bit = 0; bit < 8; ++bit)
      {
        auto corrupt = sealed;
        corrupt[i] = static_cast<char>(corrupt[i] ^ (1 << bit));
        EXPECT_THROW(open(corrupt.data(), corrupt.size()), ProtocolError)
            << "byte " << i << " bit " << bit;
      }
    }
  }
}

TEST(Envelope, RejectsMalformedInput)
{
  const std::string garbage = "hello, not msgpack";
  EXPECT_THROW(open(garbage.data(), garbage.size()), ProtocolError);
  EXPECT_THROW(open(garbage.data(), 0), ProtocolError);

  const auto sealed = bytes(seal(Status::Ok, 1));
  EXPECT_THROW(open(sealed.data(), sealed.size() - 1), ProtocolError);

  // The pre-envelope wire format: [payload-as-string, crc]
  msgpack::sbuffer legacy;
  msgpack::pack(legacy, std::make_tuple(std::string("x"), 42U));
  EXPECT_THROW(open(legacy.data(), legacy.size()), ProtocolError);
}

TEST(Envelope, RejectsOtherWireVersionsAndStatuses)
{
  auto sealed = bytes(seal(Status::Ok, 1));
  // [fixarray header][version][status]...
  sealed[1] = static_cast<char>(zRPC::detail::kWireVersion + 1);
  EXPECT_THROW(open(sealed.data(), sealed.size()), ProtocolError);

  sealed = bytes(seal(Status::Ok, 1));
  sealed[2] = 9;
  EXPECT_THROW(open(sealed.data(), sealed.size()), ProtocolError);
}

TEST(Envelope, RejectsHugeArrayClaimsWithoutAllocating)
{
  // An array32 header claiming 0xFFFFFFFF elements used to make msgpack
  // allocate ~128 GiB up front
  msgpack::sbuffer bomb;
  msgpack::packer<msgpack::sbuffer> pk(bomb);
  pk.pack_array(0xFFFFFFFFU);
  EXPECT_THROW(open(bomb.data(), bomb.size()), ProtocolError);

  // ...and the same claim hidden inside a valid, correctly CRC'd envelope
  const auto sealed = bytes(sealPayload(Status::Ok, bomb));
  EXPECT_THROW(open(sealed.data(), sealed.size()), ProtocolError);
}
