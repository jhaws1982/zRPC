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
void zRPCServer::bind(const std::string &name, F func)
{
  insertFunc<F>(name, func, typename support::callable_traits<F>::f_rtn());
}

/**
 * @brief Insert non-void returning function into RPC map
 *
 * @tparam F Callable type to bind (auto-detected by compiler)
 * @param name Name of the RPC
 * @param func Function to call
 */
template <typename F>
void zRPCServer::insertFunc(const std::string &name,
                            F func,
                            support::nonvoid_rtn const &)
{
  m_rpcs[name] = [func, name](msgpack::object const &args)
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

    /// @todo Ensure types of arguments match

    // Call the function
    typename support::typeArgs<F> realArgs;
    args.convert(realArgs);
    auto zone = std::make_unique<msgpack::zone>();
    auto rtnval = support::call(func, realArgs);
    auto rtnobj = msgpack::object(rtnval, *zone);

    return std::make_unique<msgpack::object_handle>(rtnobj, std::move(zone));
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
void zRPCServer::insertFunc(const std::string &name,
                            F func,
                            support::void_rtn const &)
{
  m_rpcs[name] = [func, name](msgpack::object const &args)
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

    /// @todo Ensure types of arguments match

    // Call the function
    typename support::typeArgs<F> realArgs;
    args.convert(realArgs);
    support::call(func, realArgs);

    return std::make_unique<msgpack::object_handle>();
  };
}

}  // namespace zRPC