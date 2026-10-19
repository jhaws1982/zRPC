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
#include "zRPC.hpp"

namespace zRPC
{
namespace detail
{
namespace
{
/**
 * @brief Number of fields in an envelope: version, status, payload, CRC
 */
constexpr std::uint32_t kEnvelopeFields = 4;

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
 * @brief CRC of an envelope: the version and status bytes, then the payload,
 * so a corrupted status cannot turn a value into an error or vice versa
 *
 * @param[in] status Status byte
 * @param[in] payload Payload bytes
 * @param[in] size Number of payload bytes
 * @return std::uint32_t CRC-32 of the header and payload
 */
std::uint32_t envelopeCrc(std::uint8_t status,
                          const char *payload,
                          std::size_t size)
{
  const std::uint8_t header[2] = {kWireVersion, status};
  const auto crc = crcUpdate(0xFFFFFFFFU, header, sizeof(header));
  return crcUpdate(crc, payload, size) ^ 0xFFFFFFFFU;
}
}  // namespace

std::uint32_t crc32(const void *data, std::size_t size)
{
  return crcUpdate(0xFFFFFFFFU, data, size) ^ 0xFFFFFFFFU;
}

msgpack::sbuffer sealPayload(Status status, const msgpack::sbuffer &payload)
{
  const auto statusByte = static_cast<std::uint8_t>(status);
  const auto size = static_cast<std::uint32_t>(payload.size());

  msgpack::sbuffer out;
  msgpack::packer<msgpack::sbuffer> pk(out);
  pk.pack_array(kEnvelopeFields);
  pk.pack_uint8(kWireVersion);
  pk.pack_uint8(statusByte);
  pk.pack_bin(size);
  pk.pack_bin_body(payload.data(), size);
  pk.pack_uint32(envelopeCrc(statusByte, payload.data(), payload.size()));
  return out;
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
      field[3].type != msgpack::type::POSITIVE_INTEGER)
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

  const auto status = static_cast<std::uint8_t>(field[1].via.u64);
  const auto &payload = field[2].via.bin;
  if (envelopeCrc(status, payload.ptr, payload.size) != field[3].via.u64)
  {
    throw ProtocolError("CRC mismatch: message corrupted in transit");
  }

  Opened opened;
  opened.status = static_cast<Status>(status);
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
}  // namespace zRPC
