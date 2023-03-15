/*
 * @file   zRPCServer.inl
 * @author Jonathan Haws
 * @date   18-Dec-2021 1:34:00 pm
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
template <typename F>
void Server::bind(const std::string &name, F func)
{
  if (m_rpcs.find(name) == m_rpcs.end())
  {
    insertFunc<F>(name, func, typename support::callable_traits<F>::f_rtn());
  }
  else
  {
    throw std::runtime_error("'" + name +
                             "' has already been registered as an RPC.");
  }
}

/**
 * @brief Insert non-void returning function into RPC map
 *
 * @tparam F Callable type to bind (auto-detected by compiler)
 * @param name Name of the RPC
 * @param func Function to call
 */
template <typename F>
void Server::insertFunc(const std::string &name,
                        F func,
                        support::nonvoid_rtn const &)
{
  m_rpcs[name] = [this, func, name](msgpack::object const &args)
  {
    auto called_args = args.via.array.size;
    auto expected_args = std::tuple_size<support::typeArgs<F>>::value;

    // Ensure number of arguments matches
    if (called_args != expected_args)
    {
      throw std::runtime_error(
          "Function " + name + " called with " + std::to_string(called_args) +
          " arguments; expected " + std::to_string(expected_args));
    }

    // Call the function
    typename support::typeArgs<F> realArgs;
    args.convert(realArgs);
    auto zone = std::make_unique<msgpack::zone>();
    auto rtnval = support::call(func, realArgs);
    auto rtnobj = msgpack::object(rtnval, *zone);

    msgpack::object_handle hdl(rtnobj, std::move(zone));
    std::uint32_t crc = CRC::Calculate(hdl.get().via.bin.ptr,
                                       hdl.get().via.bin.size, m_crcTable);
    return std::make_unique<ReturnType>(std::make_tuple(hdl.get(), crc));
  };
}

/**
 * @brief Insert void returning function into RPC map
 *
 * @tparam F Callable type to bind (auto-detected by compiler)
 * @param name Name of the RPC
 * @param func Function to call
 */
template <typename F>
void Server::insertFunc(const std::string &name,
                        F func,
                        support::void_rtn const &)
{
  m_rpcs[name] = [this, func, name](msgpack::object const &args)
  {
    auto called_args = args.via.array.size;
    auto expected_args = std::tuple_size<support::typeArgs<F>>::value;

    // Ensure number of arguments matches
    if (called_args != expected_args)
    {
      throw std::runtime_error(
          "Function " + name + " called with " + std::to_string(called_args) +
          " arguments; expected " + std::to_string(expected_args));
    }

    // Call the function
    typename support::typeArgs<F> realArgs;
    args.convert(realArgs);
    support::call(func, realArgs);

    msgpack::object_handle hdl;
    std::uint32_t crc = CRC::Calculate(hdl.get().via.bin.ptr,
                                       hdl.get().via.bin.size, m_crcTable);
    return std::make_unique<ReturnType>(std::make_tuple(hdl.get(), crc));
  };
}

}  // namespace zRPC