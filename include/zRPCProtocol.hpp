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

#include <array>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <msgpack.hpp>
#pragma GCC diagnostic pop

namespace zRPC
{
/**
 * @brief Identifies one call. Every resend of a call carries the same ID, so
 * the server can recognise it.
 */
struct RequestId
{
  /**
   * @brief 128 bits: a random per-Client prefix and a per-call counter
   */
  std::array<std::uint8_t, 16> bytes{};

  bool operator==(const RequestId &) const = default;

  /**
   * @brief The ID as 32 hexadecimal digits
   */
  std::string toString() const;
};

namespace detail
{
/**
 * @brief Hash of a RequestId, for unordered containers
 */
struct RequestIdHash
{
  std::size_t operator()(const RequestId &id) const noexcept
  {
    std::uint64_t prefix = 0;
    std::uint64_t counter = 0;
    std::memcpy(&prefix, id.bytes.data(), sizeof(prefix));
    std::memcpy(&counter, id.bytes.data() + sizeof(prefix), sizeof(counter));
    return static_cast<std::size_t>(prefix ^ (counter * 0x9E3779B97F4A7C15ULL));
  }
};

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
 * @brief Whether a sealed message is small enough for peers to accept
 *
 * Every zRPC socket drops a peer that sends more than kMaxMessageSize, so a
 * larger message would vanish without an error.
 *
 * @param[in] sealed Sealed message
 * @return bool True if the message is within kMaxMessageSize
 */
inline bool fitsMessageLimit(const msgpack::sbuffer &sealed)
{
  return sealed.size() <= static_cast<std::size_t>(kMaxMessageSize);
}

/**
 * @brief Describe a message that is over the size limit
 *
 * @param[in] what What the message is, e.g. "Reply to 'f'"
 * @param[in] sealed The message
 * @return std::string Text for an error
 */
inline std::string tooLarge(const std::string &what,
                            const msgpack::sbuffer &sealed)
{
  return what + " is " + std::to_string(sealed.size()) +
         " bytes, more than the " + std::to_string(kMaxMessageSize) +
         "-byte message limit";
}

/**
 * @brief Deepest nesting of arrays and maps accepted from a peer
 */
inline constexpr std::size_t kMaxDepth = 64;

/**
 * @brief Convert a timeout to 0MQ's form: int milliseconds, -1 = forever.
 * Negative timeouts and any too long for an int (about 24 days) mean forever.
 *
 * @param[in] timeout Timeout to convert
 * @return int Milliseconds for ZMQ_RCVTIMEO and similar options
 */
inline int zmqTimeout(std::chrono::milliseconds timeout)
{
  const auto ms = timeout.count();
  return (ms < 0 || ms > INT_MAX) ? -1 : static_cast<int>(ms);
}

/**
 * @brief CRC-32/ISO-HDLC of a block of bytes (check value 0xCBF43926)
 *
 * @param[in] data Bytes to checksum
 * @param[in] size Number of bytes
 * @return std::uint32_t CRC-32 of the bytes
 */
std::uint32_t crc32(const void *data, std::size_t size);

/**
 * @brief Wrap an already packed payload in an envelope
 *
 * Every request, reply and publication is one MessagePack array
 * `[version, status, id, payload, crc32]`:
 * - `id` is the request ID as 16 bytes of binary data in requests and their
 *   replies, and empty in publications;
 * - `payload` is the packed body as binary data;
 * - `crc32` (CRC-32/ISO-HDLC) covers the version, status, id and payload
 *   bytes.
 *
 * The fields before the payload, and the CRC, always use the same encoding
 * (uint8, uint8, bin8, ..., uint32), so peek() can read the ID and CRC at
 * fixed offsets without decoding the message.
 *
 * @param[in] status Whether the payload is a value or a zRPC::Error
 * @param[in] payload Packed body
 * @param[in] id Request ID, if the envelope is a request or a reply
 * @return msgpack::sbuffer Envelope ready to send
 */
msgpack::sbuffer sealPayload(Status status,
                             const msgpack::sbuffer &payload,
                             const std::optional<RequestId> &id = std::nullopt);

/**
 * @brief Pack a body and wrap it in an envelope
 *
 * @tparam T MessagePack-able body type
 * @param[in] status Whether the body is a value or a zRPC::Error
 * @param[in] body Body to send
 * @param[in] id Request ID, if the envelope is a request or a reply
 * @return msgpack::sbuffer Envelope ready to send
 */
template <class T>
msgpack::sbuffer seal(Status status,
                      const T &body,
                      const std::optional<RequestId> &id = std::nullopt)
{
  msgpack::sbuffer payload;
  msgpack::pack(payload, body);
  return sealPayload(status, payload, id);
}

/**
 * @brief The fields of an envelope that can be read without decoding it
 */
struct Peeked
{
  /**
   * @brief Request ID
   */
  RequestId id;

  /**
   * @brief The envelope's CRC, which identifies its exact contents
   */
  std::uint32_t crc{0};
};

/**
 * @brief Read the request ID and CRC of an envelope, and check the CRC,
 * without decoding the envelope
 *
 * Cheap enough for the server's broker thread, and safe to trust: a message
 * corrupted in transit cannot lay claim to another call's ID. open() still
 * has to validate the payload before it is used.
 *
 * @param[in] data Received bytes
 * @param[in] size Number of received bytes
 * @return std::optional<Peeked> The ID and CRC, or nothing if the message
 * does not have sealPayload()'s layout, carries no request ID, or fails its
 * CRC check
 */
std::optional<Peeked> peek(const void *data, std::size_t size);

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
   * @brief Request ID, for requests and their replies
   */
  std::optional<RequestId> id;

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
