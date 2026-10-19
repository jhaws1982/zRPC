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

#ifndef ZRPC_HPP_
#define ZRPC_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <zmq.hpp>
#include "zRPCProtocol.hpp"
#include "zRPCSupport.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <msgpack.hpp>
#pragma GCC diagnostic pop

namespace zRPC
{
/**
 * @brief Why the server could not return a value for a call
 */
enum class ErrorCode : std::uint8_t
{
  /**
   * @brief Unclassified failure
   */
  Unknown = 0,

  /**
   * @brief No RPC is bound under the requested name
   */
  NotFound = 1,

  /**
   * @brief The RPC was called with the wrong number or types of arguments
   */
  InvalidArguments = 2,

  /**
   * @brief The bound function threw an exception
   */
  Failed = 3,

  /**
   * @brief The request was corrupt or not understood (CRC mismatch,
   * malformed message, or another wire version)
   */
  BadRequest = 4,
};
}  // namespace zRPC

MSGPACK_ADD_ENUM(zRPC::ErrorCode);

namespace zRPC
{
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

  /**
   * @brief Category of the error
   */
  ErrorCode m_code{ErrorCode::Unknown};

  MSGPACK_DEFINE(m_msg, m_code)
};

/**
 * @class Exception zRPC.hpp "zRPC.hpp"
 *
 * @brief Base class of the exceptions zRPC throws for failed calls
 */
class Exception : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

/**
 * @class RemoteError zRPC.hpp "zRPC.hpp"
 *
 * @brief The server reported an error instead of returning a value
 */
class RemoteError : public Exception
{
public:
  /**
   * @brief Construct a new zRPC::RemoteError from the server's error
   *
   * @param[in] err Error reported by the server
   */
  explicit RemoteError(Error err) : Exception(err.m_msg), m_err(std::move(err))
  {
  }

  /**
   * @brief Error reported by the server
   */
  const Error &error() const noexcept
  {
    return m_err;
  }

  /**
   * @brief Category of the error reported by the server
   */
  ErrorCode code() const noexcept
  {
    return m_err.m_code;
  }

private:
  Error m_err;
};

/**
 * @class TimeoutError zRPC.hpp "zRPC.hpp"
 *
 * @brief No reply arrived within the call's timeout
 */
class TimeoutError : public Exception
{
public:
  using Exception::Exception;
};

/**
 * @class ProtocolError zRPC.hpp "zRPC.hpp"
 *
 * @brief A message was corrupt or not understood: CRC mismatch, malformed
 * data, or a peer speaking another wire version
 */
class ProtocolError : public Exception
{
public:
  using Exception::Exception;
};

/**
 * @brief Severity of a message zRPC logs
 */
enum class LogLevel : std::uint8_t
{
  /**
   * @brief A problem zRPC recovered from, e.g. a malformed publication that
   * was dropped
   */
  Warning,

  /**
   * @brief A failure, e.g. a subscription callback that threw, or a server or
   * subscription thread that stopped unexpectedly
   */
  Error,
};

/**
 * @brief Receives the messages zRPC logs
 */
using LogHandler =
    std::function<void(LogLevel level, const std::string &message)>;

/**
 * @brief Route zRPC's log messages to a handler
 *
 * zRPC only logs problems that happen on its own threads (the server's broker
 * and workers, and subscriptions), where there is no caller to throw to.
 * Everything else is reported by exceptions. By default messages go to
 * std::cerr; an empty handler discards them. The handler may be called from
 * several threads at once.
 *
 * @param[in] handler New handler
 * @return LogHandler The previous handler, e.g. to restore later
 */
LogHandler setLogHandler(LogHandler handler);

namespace detail
{
/**
 * @brief Pass a message to the current log handler
 *
 * @param[in] level Severity of the message
 * @param[in] message Text of the message
 */
void log(LogLevel level, const std::string &message);
}  // namespace detail

/**
 * @brief Timeout that never expires
 *
 * A call made with it waits for its reply forever, so it hangs if the server
 * is down.
 */
inline constexpr std::chrono::milliseconds kWaitForever{-1};

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
   * @brief Zero-MQ RPC broker back-end socket (ROUTER) for the workers
   */
  zmq::socket_t m_brokerBackend;

  /**
   * @brief Endpoint the front-end socket is bound to
   */
  std::string m_endpoint;

  /**
   * @brief Vector of thread handler for main worker threads
   */
  std::vector<std::thread> m_th;

  /**
   * @brief Number of worker threads start() creates
   */
  uint32_t m_nWorkers;

  /**
   * @brief Flag indicating that the server is currently running
   */
  std::atomic<bool> m_running{false};

  /**
   * @brief Flag indicating that start() has been called; the RPC map is
   * read-only from then on
   */
  std::atomic<bool> m_started{false};

  /**
   * @brief Worker thread function
   */
  void worker(void);

  /**
   * @brief Decode a request, run the requested RPC and build its reply
   *
   * @param[in] msg Request received from the client
   * @return msgpack::sbuffer Reply envelope carrying the RPC's value, or a
   * zRPC::Error saying why there is none
   */
  msgpack::sbuffer process(const zmq::message_t &msg);

  /**
   * @brief Shut down the 0MQ context, unblocking the broker and all workers
   */
  void shutdown(void);

  /**
   * @brief Reply to client with identity on provided socket with provided
   * result
   *
   * @param[in] sock Socket to reply on
   * @param[in] identity Client identity to reply to
   * @param[in] res Reply envelope to send
   */
  void reply(zmq::socket_t &sock,
             zmq::message_t &identity,
             const msgpack::sbuffer &res) const;

public:
  /**
   * @brief Construct a new zRPC::Server object listening on all interfaces on
   * the specified port with the specified number of worker threads
   *
   * @warning There is no authentication: anyone who can reach the port can
   * call every bound RPC. To listen on one interface only, use the URI
   * constructor, e.g. "tcp://127.0.0.1:12345".
   *
   * @param[in] port Port to listen on
   * @param[in] nWorkers Number of worker threads to create, default = 16
   * @throws zmq::error_t if the port cannot be bound
   */
  explicit Server(const uint16_t port, const uint32_t nWorkers = 16U);

  /**
   * @brief Construct a new zRPC::Server object listening on the specified
   * address and port with the specified number of worker threads
   *
   * @param[in] uri Zero-MQ address:port to bind listening socket to.
   * @param[in] nWorkers Number of worker threads to create, default = 16
   * @throws zmq::error_t if the URI is invalid or cannot be bound
   */
  explicit Server(const std::string &uri, const uint32_t nWorkers = 16U);

  ~Server();

  /**
   * @brief Endpoint the server is listening on, with a wildcard port
   * resolved, e.g. "tcp://127.0.0.1:41234" after binding "tcp://127.0.0.1:*"
   */
  const std::string &endpoint(void) const noexcept;

  /**
   * @brief Start the worker threads and run the broker that hands client
   * requests to idle workers
   *
   * Blocks until stop() is called. Bind every RPC before calling this.
   *
   * @throws std::logic_error if the server has already been started
   */
  void start(void);

  /**
   * @brief Stop the RPC server, making start() return
   *
   * May be called from any thread, including from a function bound with
   * bind(). In that case the server stops once that RPC's reply has been
   * sent, which lets a client shut the server down remotely if you choose to
   * offer that, for example:
   * @code
   * srv.bind("terminate", [&srv]() { srv.stop(); });
   * @endcode
   */
  void stop(void);

  /**
   * @brief Bind a function to an RPC name
   *
   * Bound functions run on the worker threads, so one may be running for
   * several clients at once. A function object keeps its state between calls
   * and must synchronise any state it changes.
   *
   * @tparam F Callable type to bind (auto-detected by compiler)
   * @param[in] name Name of the RPC
   * @param[in] func Callable object to bind to the RPC name
   * @throws std::logic_error if the server has already been started
   * @throws std::runtime_error if the name is already bound
   */
  template <typename F>
  void bind(const std::string &name, F func);

  /**
   * @brief Bind a member function of an object to an RPC name
   *
   * The object must outlive the server.
   * @code
   * srv.bind("add", &Calculator::add, &calc);
   * @endcode
   *
   * @tparam C Class of the object
   * @tparam R Return type of the member function
   * @tparam A Argument types of the member function
   * @param[in] name Name of the RPC
   * @param[in] method Member function to call
   * @param[in] object Object to call it on
   * @throws std::logic_error if the server has already been started
   * @throws std::runtime_error if the name is already bound
   */
  template <typename C, typename R, typename... A>
  void bind(const std::string &name, R (C::*method)(A...), C *object);

  /**
   * @brief Bind a const member function of an object to an RPC name
   *
   * @copydetails bind(const std::string &, R (C::*)(A...), C *)
   */
  template <typename C, typename R, typename... A>
  void bind(const std::string &name,
            R (C::*method)(A...) const,
            const C *object);

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
 * arguments, with the first argument always being the name of the RPC. A call
 * either returns the RPC's value or throws, so a value and an error can never
 * be mistaken for each other:
 * @code
 * try
 * {
 *   int sum = client.call("add", 2, 3).get().as<int>();
 * }
 * catch (const zRPC::RemoteError &e)
 * {
 *   // e.code() == zRPC::ErrorCode::NotFound, InvalidArguments, Failed, ...
 * }
 * @endcode
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
   * @brief Name of this client, used in diagnostics
   */
  std::string m_identity;

  /**
   * @brief URI of the server (protocol and address:port) to connect to on each
   * RPC call
   */
  std::string m_uri;

  /**
   * @brief Timeout used by calls that do not specify one
   */
  std::chrono::milliseconds m_timeout;

  /**
   * @brief Random first half of this client's request IDs, so that IDs from
   * different clients and processes do not collide
   */
  std::uint64_t m_idPrefix;

  /**
   * @brief Second half of the next request ID
   */
  std::atomic<std::uint64_t> m_nextCall{0};

  /**
   * @brief A request ID no other call has used
   */
  RequestId newRequestId(void);

public:
  /**
   * @brief Construct a new zRPC::Client object
   *
   * Each call connects to the server separately and 0MQ assigns every
   * connection a routing ID that is unique on the server, so any number of
   * clients (in any number of processes) may share the same identity string.
   *
   * @param[in] identity Name of the client, used in diagnostics.
   * @param[in] uri Zero-MQ address:port to bind listening socket to.
   * @param[in] timeout Timeout for calls that do not pass their own, default
   * 30 s, or zRPC::kWaitForever.
   */
  explicit Client(const std::string &identity,
                  const std::string &uri,
                  std::chrono::milliseconds timeout = std::chrono::seconds(30));

  /**
   * @brief Call the RPC with the given name and given arguments, using the
   * client's default timeout
   *
   * @tparam A Variadic argument list
   * @param[in] name Name of the RPC to call on the remote server
   * @param[in] args Variadic argument list to pass to the remote server
   * @return msgpack::object_handle Value returned by the RPC (nil for a void
   * function)
   * @throws zRPC::RemoteError if the server reported an error
   * @throws zRPC::TimeoutError if no reply arrived in time
   * @throws zRPC::ProtocolError if the reply was corrupt or not understood
   * @throws zmq::error_t on a 0MQ failure, e.g. an invalid URI
   */
  template <typename... A>
  msgpack::object_handle call(const std::string &name, A... args);

  /**
   * @brief Call the RPC with the given name and given arguments
   *
   * @note A request that times out may still have reached the server, which
   * then runs the RPC (with any side effects) and discards the reply.
   *
   * @code
   * using namespace std::chrono_literals;
   * client.call(500ms, "add", 2, 3);
   * @endcode
   *
   * @tparam A Variadic argument list
   * @param[in] timeout Time to wait for the reply, or zRPC::kWaitForever
   * @param[in] name Name of the RPC to call on the remote server
   * @param[in] args Variadic argument list to pass to the remote server
   * @return msgpack::object_handle Value returned by the RPC (nil for a void
   * function)
   * @throws zRPC::RemoteError if the server reported an error
   * @throws zRPC::TimeoutError if no reply arrived in time
   * @throws zRPC::ProtocolError if the reply was corrupt or not understood
   * @throws zmq::error_t on a 0MQ failure, e.g. an invalid URI
   */
  template <typename... A>
  msgpack::object_handle call(std::chrono::milliseconds timeout,
                              const std::string &name,
                              A... args);

  /**
   * @brief Deleted: pass the timeout as a std::chrono duration, e.g. 500ms
   *
   * A plain integer timeout made `call(0, "name")` ambiguous, since 0 also
   * converts to a std::string RPC name. Deleting this overload turns old
   * `call(100, "name", ...)` code into a compile error instead.
   */
  template <typename... A>
  msgpack::object_handle call(int timeout,
                              const std::string &name,
                              A... args) = delete;
};

/**
 * @class Publisher zRPC.hpp "zRPC.hpp"
 *
 * @brief Implements a 0MQ-based message publisher.
 *
 * This creates a simple 0MQ publisher with a method to publish any
 * MessagePack-able object to a named topic.
 */
class Publisher
{
private:
  // Delete copy constructor
  Publisher(Publisher const &) = delete;

  /**
   * @brief Zero-MQ context for the publisher
   */
  zmq::context_t m_ctx;

  /**
   * @brief Publisher socket
   */
  zmq::socket_t m_pub;

  /**
   * @brief Endpoint the publisher socket is bound to
   */
  std::string m_endpoint;

  /**
   * @brief Serialises use of the publisher socket, which 0MQ does not allow
   * from several threads at once
   */
  std::mutex m_mutex;

public:
  /**
   * @brief Construct a new zRPC::Publisher object listening on all interfaces
   * on the specified port for new TCP subscriptions
   *
   * @param[in] port Port to listen on
   * @throws zmq::error_t if the port cannot be bound
   */
  explicit Publisher(const uint16_t port);

  /**
   * @brief Construct a new zRPC::Publisher object listening on the specified
   * URI for new subscriptions
   *
   * @param[in] uri Zero-MQ address/port URI to bind listening socket to.
   * @throws zmq::error_t if the URI is invalid or cannot be bound
   */
  explicit Publisher(const std::string &uri);

  /**
   * @brief Endpoint the publisher is listening on, with a wildcard port
   * resolved, e.g. "tcp://127.0.0.1:41234" after binding "tcp://127.0.0.1:*"
   */
  const std::string &endpoint(void) const noexcept;

  /**
   * @brief Publish a MessagePack-able object using the given topic name
   *
   * Safe to call from several threads at once.
   *
   * @tparam T MessagePack-able object type
   * @param[in] topic Name of the topic to publish to
   * @param[in] data Data object to publish
   * @throws zmq::error_t on a 0MQ failure
   */
  template <class T>
  void publish(const std::string &topic, const T &data);
};

class Subscriber
{
public:
  /**
   * @brief Callback alias declartion, specifying the required prototype
   *
   * @tparam T MessagePack-able object type of message to receive
   */
  template <class T>
  using cb_type = std::function<void(const std::string &topic, T &data)>;

private:
  // Delete copy constructor
  Subscriber(Subscriber const &) = delete;

  /**
   * @brief Zero-MQ context for the subscriber
   */
  zmq::context_t m_ctx;

  /**
   * @brief Vector of subscription threads
   */
  std::vector<std::thread> m_handlers;

  /**
   * @brief Guards m_handlers against concurrent subscribe() calls
   */
  std::mutex m_handlersMutex;

  /**
   * @brief Flag indicating whether the zRPC::Subscriber object is running
   */
  std::atomic<bool> m_running{false};

  /**
   * @brief Subscription handler function
   *
   * @tparam T MessagePack-able object type
   * @param[in] uri URI of the publisher to subcribe to
   * @param[in] topic Name of the topic to subscribe to
   * @param[in] cb Callback to call when message is received
   */
  template <class T>
  void handler(const std::string &uri, const std::string &topic, cb_type<T> cb);

public:
  /**
   * @brief Construct a new zRPC::Subscriber object to subscribe to messages
   * from a zRPC::Publisher
   */
  Subscriber();

  ~Subscriber();

  /**
   * @brief Subscribe to a topic on the publisher at the provided URI and call
   * the callback function for each message received.
   *
   * Safe to call from several threads at once. Each subscription runs its
   * callback on its own thread.
   *
   * @note In order to use this function properly, the received object type must
   * be specified in the call, like this:
   * @code
   * s.subscribe<string>("tcp://localhost:54321", "MyStringTopic",
   *             ^^^^^^  [](const string &topic, const string &msg)
   *                     {
   *                       cout << topic << endl;
   *                       cout << msg << endl;
   *                     });
   * @endcode
   *
   * @tparam T MessagePack-able object type
   * @param[in] uri URI of the publisher to subcribe to
   * @param[in] topic Name of the topic to subscribe to, matched exactly. An
   * empty topic receives every topic.
   * @param[in] cb Callback to call when message is received
   */
  template <class T>
  void subscribe(const std::string &uri,
                 const std::string &topic,
                 cb_type<T> cb);
};

}  // namespace zRPC

#include "zRPCClient.inl"
#include "zRPCPublisher.inl"
#include "zRPCServer.inl"
#include "zRPCSubscriber.inl"

#endif  // ZRPC_HPP_
