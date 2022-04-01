/*
 * @file   zRPC.hpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 2:57:12 pm
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

#ifndef _ZRPC_HPP_
#define _ZRPC_HPP_

#include <CRC.h>
#include <functional>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <zmq.hpp>
#include "zRPCSupport.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <msgpack.hpp>
#pragma GCC diagnostic pop

namespace zRPC
{

/**
 * @class Server zRPC.hpp "zRPC.hpp"
 *
 * @brief Implements a 0MQ-based server.
 *
 * This maintains a database of bound functions representing the remote
 * procedure calls, indexed by name. Functions must be bound before the server
 * is started to ensure that the RPC is available when the client connects. Once
 * all RPCs are bound, the `start` function will start the server listening.
 */
class Server
{
private:
  using functor_type = std::function<std::unique_ptr<msgpack::object_handle>(
      msgpack::object const &)>;

  /**
   * @brief Map of bound RPC function calls
   */
  std::unordered_map<std::string, functor_type> m_rpcs;

  /**
   * @brief Zero-MQ context for the server
   */
  zmq::context_t m_ctx;

  /**
   * @brief Zero-MQ RPC broker front-end socket (ROUTER)
   */
  zmq::socket_t m_brokerFrontend;

  /**
   * @brief Zero-MQ RPC broker back-end socket (DEALER)
   */
  zmq::socket_t m_brokerBackend;

  /**
   * @brief Vector of thread handler for main worker threads
   */
  std::vector<std::thread> m_th;

  /**
   * @brief Flag indicating that the server is currently running
   */
  bool m_running{false};

  /**
   * @brief CRC table for use in efficient CRC calculations
   */
  CRC::Table<std::uint32_t, 32> m_crcTable;

  /**
   * @brief Worker thread function
   */
  void worker(void);

  /**
   * @brief Reply to client with identity on provided socket with provided
   * result
   *
   * @param[in] sock Socket to reply on
   * @param[in] identity Client identity to reply to
   * @param[in] res MsgPack object to reply with
   */
  void reply(zmq::socket_t &sock,
             zmq::message_t &identity,
             std::unique_ptr<msgpack::object_handle> &res) const;

public:
  /**
   * @brief Construct a new zRPC::Server object listening on all interfaces on
   * the specified port with the specified number of worker threads
   *
   * @param[in] port Port to listen on
   * @param[in] nWorkers Number of worker threads to create, default = 16
   */
  explicit Server(const uint16_t port, const uint32_t nWorkers = 16U);

  /**
   * @brief Construct a new zRPC::Server object listening on the specified
   * address and port with the specified number of worker threads
   *
   * @param[in] uri Zero-MQ address:port to bind listening socket to.
   * @param[in] nWorkers Number of worker threads to create, default = 16
   */
  explicit Server(const std::string &uri, const uint32_t nWorkers = 16U);

  ~Server();

  /**
   * @brief Start listening for client connections and setup worker pool
   */
  void start(void);

  /**
   * @brief Stop the RPC server
   */
  void stop(void);

  /**
   * @brief Bind a function to an RPC name
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param[in] name Name of the RPC
   * @param[in] func Callable object to bind to the RPC name
   */
  template <typename F>
  void bind(const std::string &name, F func);

private:
  /**
   * @brief Insert non-void returning function into RPC map
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param[in] name Name of the RPC
   * @param[in] func Function to call
   * @param support::void_rtn type to differentiate insert from non-void
   */
  template <typename F>
  void insertFunc(const std::string &name,
                  F func,
                  support::nonvoid_rtn const &);

  /**
   * @brief Insert void returning function into RPC map
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param[in] name Name of the RPC
   * @param[in] func Function to call
   * @param support::void_rtn type to differentiate insert from non-void
   */
  template <typename F>
  void insertFunc(const std::string &name, F func, support::void_rtn const &);
};

/**
 * @class Client zRPC.hpp "zRPC.hpp"
 *
 * @brief Implements a 0MQ-based client.
 *
 * This provides a mechanism to call the RPC just like a regular function with
 * arguments, with the first argument always being the name of the RPC. If the
 * arguments do not match the function on the remote server, an error is
 * returned from the server.
 */
class Client
{
private:
  // Delete copy constructor
  Client(Client const &) = delete;

  /**
   * @brief Zero-MQ context for the client
   */
  zmq::context_t m_ctx;

  /**
   * @brief Identity base string to discriminate between connections
   */
  std::string m_idBase;

  /**
   * @brief Index of the RPC call from the same client
   */
  uint64_t m_idx{0};

  /**
   * @brief URI of the server (protocol and address:port) to connect to on each
   * RPC call
   */
  std::string m_uri;

  /**
   * @brief CRC table for use in efficient CRC calculations
   */
  CRC::Table<std::uint32_t, 32> m_crcTable;

public:
  /**
   * @brief Construct a new zRPC::Client object
   *
   * This will establish the connection with the server and configure the 0MQ
   * identity for the client.
   *
   * @param[in] identity Identity string to use for the client.
   * @param[in] uri Zero-MQ address:port to bind listening socket to.
   */
  explicit Client(const std::string &identity,
                  const std::string &uri);

  /**
   * @brief Call the RPC with the given name and given arguments
   *
   * @tparam A Variadic argument list
   * @param[in] name Name of the RPC to call on the remote serveer
   * @param[in] args Variadic argument list to pass to the remote server
   * @return msgpack::object_handle MessagePack'd object handle containing
   * server response (if any)
   */
  template <typename... A>
  msgpack::object_handle call(const std::string &name, A... args);
};

/**
 * @class Error zRPC.hpp "zRPC.hpp"
 *
 * @brief Defines the zRPC error class used to report errors.
 */
struct Error
{
  /**
   * @brief Error message
   */
  std::string m_msg;

  MSGPACK_DEFINE(m_msg)
};

// class zPublisher
// class zSubscriber

}  // namespace zRPC

#include "zRPCClient.inl"
#include "zRPCServer.inl"

#endif  // _ZRPC_HPP_
