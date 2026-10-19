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
using zRPC::RequestId;
using zRPC::detail::open;
using zRPC::detail::peek;
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

namespace
{
RequestId someId()
{
  RequestId id;
  for (std::size_t i = 0; i < id.bytes.size(); ++i)
  {
    id.bytes[i] = static_cast<std::uint8_t>(0xA0 + i);
  }
  return id;
}
}  // namespace

TEST(Envelope, CarriesRequestIdsThatPeekCanRead)
{
  const auto id = someId();
  const auto sealed = bytes(seal(Status::Ok, 42, id));

  auto opened = open(sealed.data(), sealed.size());
  ASSERT_TRUE(opened.id);
  EXPECT_EQ(*opened.id, id);
  EXPECT_EQ(opened.body.get().as<int>(), 42);

  const auto peeked = peek(sealed.data(), sealed.size());
  ASSERT_TRUE(peeked);
  EXPECT_EQ(peeked->id, id);
  // The CRC is the last four bytes, big-endian
  const auto *tail =
      reinterpret_cast<const std::uint8_t *>(sealed.data() + sealed.size() - 4);
  EXPECT_EQ(peeked->crc, (std::uint32_t{tail[0]} << 24) |
                             (std::uint32_t{tail[1]} << 16) |
                             (std::uint32_t{tail[2]} << 8) | tail[3]);

  // Identical calls have identical CRCs; a different call does not
  EXPECT_EQ(peek(sealed.data(), sealed.size())->crc,
            peek(bytes(seal(Status::Ok, 42, id)).data(), sealed.size())->crc);
  const auto other = bytes(seal(Status::Ok, 43, id));
  EXPECT_NE(peek(other.data(), other.size())->crc, peeked->crc);
}

TEST(Envelope, PeekHandlesEveryPayloadSize)
{
  // bin8, bin16 and bin32 payloads
  for (const std::size_t n : {0U, 200U, 300U, 70000U})
  {
    const auto sealed = bytes(seal(Status::Ok, std::string(n, 'x'), someId()));
    const auto peeked = peek(sealed.data(), sealed.size());
    ASSERT_TRUE(peeked) << n;
    EXPECT_EQ(peeked->id, someId());
    EXPECT_FALSE(peek(sealed.data(), sealed.size() - 1)) << n;
  }
}

TEST(Envelope, PublicationsAndGarbageHaveNoRequestId)
{
  const auto pub = bytes(seal(Status::Ok, 42));
  EXPECT_FALSE(open(pub.data(), pub.size()).id);
  EXPECT_FALSE(peek(pub.data(), pub.size()));

  const std::string garbage = "hello, not msgpack at all, not even close";
  EXPECT_FALSE(peek(garbage.data(), garbage.size()));
  EXPECT_FALSE(peek(garbage.data(), 3));
}

TEST(Envelope, RequestIdPrintsAsHex)
{
  EXPECT_EQ(someId().toString(), "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
}

TEST(Envelope, DetectsEverySingleBitFlip)
{
  for (const auto status : {Status::Ok, Status::Error})
  {
    const auto sealed = bytes(seal(status, Msg{1, "hello"}, someId()));
    for (std::size_t i = 0; i < sealed.size(); ++i)
    {
      for (int bit = 0; bit < 8; ++bit)
      {
        auto corrupt = sealed;
        corrupt[i] = static_cast<char>(corrupt[i] ^ (1 << bit));
        EXPECT_THROW(open(corrupt.data(), corrupt.size()), ProtocolError)
            << "byte " << i << " bit " << bit;
        EXPECT_FALSE(peek(corrupt.data(), corrupt.size()))
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
  // [fixarray][uint8 marker][version][uint8 marker][status]...
  auto sealed = bytes(seal(Status::Ok, 1));
  sealed[2] = static_cast<char>(zRPC::detail::kWireVersion + 1);
  EXPECT_THROW(open(sealed.data(), sealed.size()), ProtocolError);
  EXPECT_FALSE(peek(sealed.data(), sealed.size()));

  sealed = bytes(seal(Status::Ok, 1));
  sealed[4] = 9;
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
