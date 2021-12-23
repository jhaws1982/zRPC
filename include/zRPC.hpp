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
 * @class zRPCServer zRPC.hpp
 *
 * @brief Implements a 0MQ-based server.
 *
 * This maintains a database of bound functions representing the remote
 * procedure calls, indexed by name. Functions must be bound before the server
 * is started to ensure that the RPC is available when the client connects. Once
 * all RPCs are bound, the `start` function will start the server listening.
 */
class zRPCServer
{
private:
  using functor_type = std::function<std::unique_ptr<msgpack::object_handle>(
      msgpack::object const &)>;
  std::unordered_map<std::string, functor_type> m_rpcs;

  /**
   * @brief Zero-MQ context for the server
   */
  zmq::context_t m_cxt;

  /**
   * @brief Zero-MQ RPC broker front-end socket (ROUTER)
   */
  zmq::socket_t m_brokerFront;

  /**
   * @brief Zero-MQ RPC broker back-end socket (DEALER)
   */
  zmq::socket_t m_brokerBack;

  /**
   * @brief Thread handler for main server thread
   */
  std::vector<std::thread> m_th;

  void worker(void);

public:
  /**
   * @brief Construct a new zRPCServer object listening on all interfaces on the
   * specified port with the specified number of worker threads
   *
   * @param[in] port Port to listen on
   * @param nWorkers[in] Number of worker threads to create, default = 16
   */
  explicit zRPCServer(const uint16_t port, const uint32_t nWorkers = 16U);

  /**
   * @brief Construct a new zRPCServer object listening on the specified address
   * and port with the specified number of worker threads
   *
   * @param[in] address Zero-MQ address to bind listening socket to.
   * @param[in] port Port to listen on
   * @param nWorkers[in] Number of worker threads to create, default = 16
   */
  zRPCServer(const std::string &address,
             const uint16_t port,
             const uint32_t nWorkers = 16U);

  /**
   * @brief Destroy the zRPCServer object and cleanly close down all 0MQ sockets
   */
  // ~zRPCServer() = default;

  /**
   * @brief Start listening for client connections and setup worker pool
   */
  void start(void);

  /**
   * @brief Bind a function to an RPC name
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param name Name of the RPC
   * @param func Callable object to bind to the RPC name
   */
  template <typename F>
  void bind(const std::string &name, F func);

  /**
   * @brief Insert non-void returning function into RPC map
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param name Name of the RPC
   * @param func Function to call
   */
  template <typename F>
  void insertFunc(const std::string &name,
                  F func,
                  support::nonvoid_rtn const &);

  /**
   * @brief Insert void returning function into RPC map
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param name Name of the RPC
   * @param func Function to call
   */
  template <typename F>
  void insertFunc(const std::string &name, F func, support::void_rtn const &);
};

class zRPCClient
{
private:
  // Delete copy constructor
  zRPCClient(zRPCClient const &) = delete;

  /**
   * @brief Zero-MQ context for the server
   */
  zmq::context_t m_cxt;

  /**
   * @brief Identity base string to discriminate between connections
   */
  std::string m_idBase;

  /**
   * @brief Index of the RPC call from the same client
   */
  uint64_t m_idx{0};

  /**
   * @brief URI of the server (address + port) to connect to on each RPC call
   */
  std::string m_uri;

public:
  /**
   * @brief Construct a new zRPCClient object
   *
   * This will establish the connection with the server and configure the 0MQ
   * identity for the client.
   *
   * @param identity Identity string to use for the client.
   * @param address Address of the server to connect to.
   * @param port Port of the server to connect to.
   */
  zRPCClient(const std::string &identity,
             const std::string &address,
             const uint16_t port);

  template <typename... A>
  msgpack::object_handle call(const std::string &name, A... args);
};

// class zPublisher
// class zSubscriber

}  // namespace zRPC

#include "zRPCClient.inl"
#include "zRPCServer.inl"

#endif  // _ZRPC_HPP_