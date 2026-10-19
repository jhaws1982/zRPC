/*
 * @file   zRPCClient.inl
 * @author Jonathan Haws
 * @date   21-Dec-2021 8:44:46 pm
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

namespace zRPC
{
template <typename... A>
msgpack::object_handle Client::call(const std::string &name, A... args)
{
  return call(m_timeout, name, args...);
}

template <typename... A>
msgpack::object_handle Client::call(std::chrono::milliseconds timeout,
                                    const std::string &name,
                                    A... args)
{
  // [name, [args...]] in a CRC-protected envelope carrying a new request ID
  const auto id = newRequestId();
  const auto request = detail::seal(
      detail::Status::Ok,
      std::forward_as_tuple(name, std::forward_as_tuple(args...)), id);
  return exchange(id, request, name, timeout);
}

template <typename... A>
msgpack::object_handle Client::call(const RequestId &id,
                                    std::chrono::milliseconds timeout,
                                    const std::string &name,
                                    A... args)
{
  // Same ID, same bytes: the server recognises the call
  const auto request = detail::seal(
      detail::Status::Ok,
      std::forward_as_tuple(name, std::forward_as_tuple(args...)), id);
  return exchange(id, request, name, timeout);
}
}  // namespace zRPC
