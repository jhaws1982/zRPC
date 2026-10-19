/*
 * @file   zRPCProtocol.hpp
 * @author Jonathan Haws
 * @date   18-Oct-2026
 *
 * @brief Wire format shared by zRPC requests, replies and publications
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

#ifndef ZRPC_PROTOCOL_HPP_
#define ZRPC_PROTOCOL_HPP_

#include <cstddef>
#include <cstdint>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <msgpack.hpp>
#pragma GCC diagnostic pop

namespace zRPC
{
namespace detail
{
/**
 * @brief Whether an envelope carries a value or a zRPC::Error
 */
enum class Status : std::uint8_t
{
  Ok = 0,
  Error = 1,
};

/**
 * @brief Version of the envelope format below. Peers reject any other
 * version instead of misreading it.
 */
inline constexpr std::uint8_t kWireVersion = 1;

/**
 * @brief Largest single message accepted from a peer, in bytes
 */
inline constexpr std::int64_t kMaxMessageSize = 64 * 1024 * 1024;

/**
 * @brief Limits applied when unpacking data received from a peer
 *
 * MessagePack allocates storage for an array or map as soon as it reads the
 * header, before checking that the elements are present, so a few hostile
 * bytes could otherwise demand gigabytes. Strings and binary data are only
 * copied once their bytes have arrived, so they are bounded by message size.
 *
 * @return msgpack::unpack_limit Limits to pass to msgpack::unpack
 */
inline msgpack::unpack_limit unpackLimits()
{
  constexpr auto maxBytes = static_cast<std::size_t>(kMaxMessageSize);
  return msgpack::unpack_limit(/* array */ 1U << 17,
                               /* map */ 1U << 16,
                               /* str */ maxBytes,
                               /* bin */ maxBytes,
                               /* ext */ maxBytes,
                               /* depth */ 16);
}

/**
 * @brief Wrap an already packed payload in an envelope
 *
 * Every request, reply and publication is one MessagePack array
 * `[version, status, payload, crc32]`, where `payload` is the packed body as
 * binary data and `crc32` (CRC-32/ISO-HDLC) covers the version byte, the
 * status byte and the payload bytes.
 *
 * @param[in] status Whether the payload is a value or a zRPC::Error
 * @param[in] payload Packed body
 * @return msgpack::sbuffer Envelope ready to send
 */
msgpack::sbuffer sealPayload(Status status, const msgpack::sbuffer &payload);

/**
 * @brief Pack a body and wrap it in an envelope
 *
 * @tparam T MessagePack-able body type
 * @param[in] status Whether the body is a value or a zRPC::Error
 * @param[in] body Body to send
 * @return msgpack::sbuffer Envelope ready to send
 */
template <class T>
msgpack::sbuffer seal(Status status, const T &body)
{
  msgpack::sbuffer payload;
  msgpack::pack(payload, body);
  return sealPayload(status, payload);
}

/**
 * @brief Contents of a validated envelope
 */
struct Opened
{
  /**
   * @brief Whether the body is a value or a zRPC::Error
   */
  Status status{Status::Ok};

  /**
   * @brief Unpacked body. It owns its memory, so it stays valid after the
   * received message is gone.
   */
  msgpack::object_handle body;
};

/**
 * @brief Validate an envelope received from a peer and unpack its body
 *
 * @param[in] data Received bytes
 * @param[in] size Number of received bytes
 * @return Opened Status and body
 * @throws zRPC::ProtocolError if the message is malformed, too large, from
 * another wire version, or fails its CRC check
 */
Opened open(const void *data, std::size_t size);

}  // namespace detail
}  // namespace zRPC

#endif  // ZRPC_PROTOCOL_HPP_
