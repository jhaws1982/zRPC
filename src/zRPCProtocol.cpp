/*
 * @file   zRPCProtocol.cpp
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

#include <array>
#include <cstring>
#include "zRPC.hpp"

namespace zRPC
{
namespace detail
{
namespace
{
/**
 * @brief Number of fields in an envelope: version, status, id, payload, CRC
 */
constexpr std::uint32_t kEnvelopeFields = 5;

/**
 * @brief Size of a request ID on the wire
 */
constexpr std::size_t kIdSize = sizeof(RequestId::bytes);

/**
 * @brief MessagePack markers of the fixed-width encodings sealPayload() uses
 */
constexpr std::uint8_t kFixArray5 = 0x95;
constexpr std::uint8_t kUint8 = 0xcc;
constexpr std::uint8_t kBin8 = 0xc4;
constexpr std::uint8_t kBin16 = 0xc5;
constexpr std::uint8_t kBin32 = 0xc6;
constexpr std::uint8_t kUint32 = 0xce;

/**
 * @brief Offset of the request ID in an envelope:
 * [array][uint8 version][uint8 status][bin8 length]
 */
constexpr std::size_t kIdOffset = 1 + 2 + 2 + 2;

/**
 * @brief Size of the encoded CRC at the end of an envelope
 */
constexpr std::size_t kCrcSize = 1 + 4;

/**
 * @brief Build the lookup table for CRC-32/ISO-HDLC (reflected polynomial
 * 0xEDB88320), the CRC used by Ethernet, zip and PNG
 *
 * @return std::array<std::uint32_t, 256> CRC of every possible byte
 */
constexpr std::array<std::uint32_t, 256> makeCrcTable()
{
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i)
  {
    std::uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit)
    {
      c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

/**
 * @brief CRC-32 lookup table, computed at compile time
 */
constexpr auto kCrcTable = makeCrcTable();

/**
 * @brief Feed more bytes into a running CRC-32 register
 *
 * @param[in] crc Register value: 0xFFFFFFFF to start, then the previous result
 * @param[in] data Bytes to add
 * @param[in] size Number of bytes
 * @return std::uint32_t Updated register; invert it to get the CRC
 */
std::uint32_t crcUpdate(std::uint32_t crc, const void *data, std::size_t size)
{
  const auto *bytes = static_cast<const unsigned char *>(data);
  for (std::size_t i = 0; i < size; ++i)
  {
    crc = kCrcTable[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
  }
  return crc;
}

/**
 * @brief CRC of an envelope: the version and status bytes, the request ID,
 * then the payload. A corrupted status cannot turn a value into an error, nor
 * a corrupted ID route a reply to the wrong call.
 *
 * @param[in] status Status byte
 * @param[in] id Request ID bytes (none for a publication)
 * @param[in] idSize Number of request ID bytes
 * @param[in] payload Payload bytes
 * @param[in] size Number of payload bytes
 * @return std::uint32_t CRC-32 of the header and payload
 */
std::uint32_t envelopeCrc(std::uint8_t status,
                          const void *id,
                          std::size_t idSize,
                          const char *payload,
                          std::size_t size)
{
  const std::uint8_t header[2] = {kWireVersion, status};
  auto crc = crcUpdate(0xFFFFFFFFU, header, sizeof(header));
  crc = crcUpdate(crc, id, idSize);
  return crcUpdate(crc, payload, size) ^ 0xFFFFFFFFU;
}
}  // namespace

std::uint32_t crc32(const void *data, std::size_t size)
{
  return crcUpdate(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU;
}

msgpack::sbuffer sealPayload(Status status,
                             const msgpack::sbuffer &payload,
                             const std::optional<RequestId> &id)
{
  const auto statusByte = static_cast<std::uint8_t>(status);
  const auto size = static_cast<std::uint32_t>(payload.size());
  const char *idBytes =
      id ? reinterpret_cast<const char *>(id->bytes.data()) : nullptr;
  const auto idSize = static_cast<std::uint32_t>(id ? kIdSize : 0);

  // Fixed-width encodings up to the payload, and for the CRC, so that peek()
  // finds the ID and CRC at fixed offsets
  msgpack::sbuffer out;
  msgpack::packer<msgpack::sbuffer> pk(out);
  pk.pack_array(kEnvelopeFields);
  pk.pack_fix_uint8(kWireVersion);
  pk.pack_fix_uint8(statusByte);
  pk.pack_bin(idSize);
  if (idSize > 0)
  {
    pk.pack_bin_body(idBytes, idSize);
  }
  pk.pack_bin(size);
  pk.pack_bin_body(payload.data(), size);
  pk.pack_fix_uint32(
      envelopeCrc(statusByte, idBytes, idSize, payload.data(), payload.size()));
  return out;
}

std::optional<Peeked> peek(const void *data, std::size_t size)
{
  constexpr std::size_t kPayloadOffset = kIdOffset + kIdSize;
  const auto *bytes = static_cast<const std::uint8_t *>(data);
  if (size < kPayloadOffset + 2 + kCrcSize || bytes[0] != kFixArray5 ||
      bytes[1] != kUint8 || bytes[2] != kWireVersion || bytes[3] != kUint8 ||
      bytes[5] != kBin8 || bytes[6] != kIdSize ||
      bytes[size - kCrcSize] != kUint32)
  {
    return std::nullopt;
  }

  // The payload is bin8, bin16 or bin32: a marker, a big-endian length, then
  // the bytes, which must end exactly where the CRC begins
  std::size_t lengthSize = 0;
  switch (bytes[kPayloadOffset])
  {
    case kBin8:
      lengthSize = 1;
      break;
    case kBin16:
      lengthSize = 2;
      break;
    case kBin32:
      lengthSize = 4;
      break;
    default:
      return std::nullopt;
  }
  const std::size_t payloadStart = kPayloadOffset + 1 + lengthSize;
  if (size < payloadStart + kCrcSize)
  {
    return std::nullopt;
  }
  std::size_t length = 0;
  for (std::size_t i = 0; i < lengthSize; ++i)
  {
    length = (length << 8) | bytes[kPayloadOffset + 1 + i];
  }
  if (length != size - kCrcSize - payloadStart)
  {
    return std::nullopt;
  }

  const auto *tail = bytes + size - kCrcSize + 1;
  const std::uint32_t crc =
      (std::uint32_t{tail[0]} << 24) | (std::uint32_t{tail[1]} << 16) |
      (std::uint32_t{tail[2]} << 8) | std::uint32_t{tail[3]};
  if (envelopeCrc(bytes[4], bytes + kIdOffset, kIdSize,
                  reinterpret_cast<const char *>(bytes + payloadStart),
                  length) != crc)
  {
    return std::nullopt;
  }

  Peeked peeked;
  std::memcpy(peeked.id.bytes.data(), bytes + kIdOffset, kIdSize);
  peeked.crc = crc;
  return peeked;
}

Opened open(const void *data, std::size_t size)
{
  msgpack::object_handle envelope;
  try
  {
    envelope = msgpack::unpack(static_cast<const char *>(data), size, nullptr,
                               nullptr, unpackLimits());
  }
  catch (const std::exception &e)
  {
    throw ProtocolError(std::string("Malformed message: ") + e.what());
  }

  const msgpack::object &o = envelope.get();
  if (o.type != msgpack::type::ARRAY || o.via.array.size != kEnvelopeFields)
  {
    throw ProtocolError("Malformed message: not a zRPC envelope");
  }

  const msgpack::object *field = o.via.array.ptr;
  if (field[0].type != msgpack::type::POSITIVE_INTEGER ||
      field[1].type != msgpack::type::POSITIVE_INTEGER ||
      field[2].type != msgpack::type::BIN ||
      field[3].type != msgpack::type::BIN ||
      field[4].type != msgpack::type::POSITIVE_INTEGER)
  {
    throw ProtocolError("Malformed message: not a zRPC envelope");
  }
  if (field[0].via.u64 != kWireVersion)
  {
    throw ProtocolError("Unsupported zRPC wire version " +
                        std::to_string(field[0].via.u64) + " (expected " +
                        std::to_string(kWireVersion) + ")");
  }
  if (field[1].via.u64 > static_cast<std::uint8_t>(Status::Error))
  {
    throw ProtocolError("Malformed message: unknown status " +
                        std::to_string(field[1].via.u64));
  }

  const auto &id = field[2].via.bin;
  if (id.size != 0 && id.size != kIdSize)
  {
    throw ProtocolError("Malformed message: bad request ID");
  }

  const auto status = static_cast<std::uint8_t>(field[1].via.u64);
  const auto &payload = field[3].via.bin;
  if (envelopeCrc(status, id.ptr, id.size, payload.ptr, payload.size) !=
      field[4].via.u64)
  {
    throw ProtocolError("CRC mismatch: message corrupted in transit");
  }

  Opened opened;
  opened.status = static_cast<Status>(status);
  if (id.size == kIdSize)
  {
    opened.id.emplace();
    std::memcpy(opened.id->bytes.data(), id.ptr, kIdSize);
  }
  try
  {
    // Without a reference callback, unpack copies strings and binary data
    // into the new handle's zone, so the body does not point into `data`
    opened.body = msgpack::unpack(payload.ptr, payload.size, nullptr, nullptr,
                                  unpackLimits());
  }
  catch (const std::exception &e)
  {
    throw ProtocolError(std::string("Malformed payload: ") + e.what());
  }
  return opened;
}

}  // namespace detail

std::string RequestId::toString() const
{
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(2 * bytes.size());
  for (const auto b : bytes)
  {
    text += kHex[b >> 4];
    text += kHex[b & 0x0F];
  }
  return text;
}

}  // namespace zRPC
