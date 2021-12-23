/*
 * @file   zRPCSupport.hpp
 * @author Jonathan Haws
 * @date   18-Dec-2021 10:05:56 am
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

#include <type_traits>

namespace zRPC
{
namespace support
{
/**
 * @brief Primary template declaration of the callable_traits support structure
 */
template <typename T>
struct callable_traits : callable_traits<decltype(&T::operator())>
{
};

/**
 * @brief Specializations of callable_traits to handle various other functor
 * types, such as lambdas and member functions
 *
 * @tparam NS Containing namespace of the function
 * @tparam R Return type of the function
 * @tparam A Function arguments
 */
template <typename NS, typename R, typename... A>
struct callable_traits<R (NS::*)(A...)> : callable_traits<R (*)(A...)>
{
};
template <typename NS, typename R, typename... A>
struct callable_traits<R (NS::*)(A...) const> : callable_traits<R (*)(A...)>
{
};

struct nonvoid_rtn
{
};
template <typename T>
struct rtn
{
  typedef nonvoid_rtn type;
};
struct void_rtn
{
};
template <>
struct rtn<void>
{
  typedef void_rtn type;
};

/**
 * @brief Specialization to extract information about the return type, number of
 * arguments, and type of arguments
 *
 * @tparam R Return type of the function
 * @tparam A Function arguments
 */
template <typename R, typename... A>
struct callable_traits<R (*)(A...)>
{
  using return_type = R;
  using num_args = std::integral_constant<std::size_t, sizeof...(A)>;
  using type_args = std::tuple<typename std::decay<A>::type...>;

  typedef typename rtn<R>::type f_rtn;
};

/**
 * @brief Helper routine to check if return type is void
 *
 * @tparam F Functor type to check return type
 */
template <typename F>
using isVoidReturn = std::is_void<typename callable_traits<F>::return_type>;

/**
 * @brief Helper routine to get the return type of F
 *
 * @tparam F Functor type to check the return type
 */
template <typename F>
using returnType = callable_traits<F>::return_type;

/**
 * @brief Helper routine to get the number of arguments to F
 *
 * @tparam F Functor type to check number of arguments
 */
template <typename F>
using numArgs = callable_traits<F>::num_args;

/**
 * @brief Helper routine to get the types of arguments to F
 *
 * @tparam F Functor type to check types of arguments
 */
template <typename F>
using typeArgs = callable_traits<F>::type_args;

/**
 * @brief Call the function using C++17 fold expression for the arguments
 *
 * @tparam F Callable type to bind (auto-detected by compiler)
 * @tparam Args Arguments to the callable functor
 * @tparam I Index sequence into the tuple
 * @param func Functor to call
 * @param params Forwarded variadic arguments to the function
 * @return decltype(auto) Auto-detected return value of the functor
 */
template <typename F, typename... Args, std::size_t... I>
decltype(auto) call_detail(F func,
                           std::tuple<Args...> &&params,
                           std::index_sequence<I...>)
{
  return func(std::get<I>(params)...);
}

/**
 * @brief Calls a functor with arguments provided as a std::tuple
 *
 * @tparam F Callable type to bind (auto-detected by compiler)
 * @tparam Args Arguments to the callable functor
 * @param func Functor to call
 * @param args Variadic arguments to the function
 * @return decltype(auto) Auto-detected return value of the functor
 */
template <typename F, typename... Args>
decltype(auto) call(F func, std::tuple<Args...> &args)
{
  return call_detail(func, std::forward<std::tuple<Args...>>(args),
                     std::index_sequence_for<Args...>{});
}

}  // namespace support
}  // namespace zRPC