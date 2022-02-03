/*
 * @file   zRPCServer.cpp
 * @author Jonathan Haws
 * @date   30-Oct-2021 3:09:39 pm
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

#include "zRPC.hpp"

#include <csignal>
#include <iostream>

using namespace zRPC;

Server::Server(const uint16_t port, const uint32_t nWorkers) :
    Server("tcp://*", port, nWorkers)
{
}

Server::Server(const std::string &address,
               const uint16_t port,
               const uint32_t nWorkers) :
    m_ctx(16),
    m_brokerFrontend(m_ctx, ZMQ_ROUTER),
    m_brokerBackend(m_ctx, ZMQ_DEALER)
{
  try
  {
    // Start the broker sockets and bind to their ports
    m_brokerFrontend.bind(address + ":" + std::to_string(port));
    m_brokerBackend.bind("inproc://backend");
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Error " << e.num() << ": " << e.what() << std::endl;
  }

  m_running = true;
  for (uint32_t n = 0; n < nWorkers; ++n)
  {
    // Create and connect the worker sockets now
    m_th.emplace_back(std::thread([this]() { worker(); }));
  }
}

Server::~Server()
{
  // Ensure we have shut things down completely
  stop();

  // Loop over all worker threads and join them
  for (auto &t : m_th)
  {
    if (t.joinable())
    {
      t.join();
    }
  }
}

void Server::start(void)
{
  // Start the proxy to connect multiple clients to multiple workers
  try
  {
    zmq::proxy(m_brokerFrontend, m_brokerBackend);
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Proxy Error " << e.num() << ": " << e.what()
              << std::endl;
  }
}

void Server::stop(void)
{
  // HACK: 0MQ does not have a way to flush output buffers, so to terminate
  // properly we need to have a short delay to ensure that the buffers are
  // cleared before shutting down the context.
  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  // Clear the running flag and shutdown the context. Final cleanup will take
  // place in the destructor.
  m_running = false;
  m_ctx.shutdown();
}

void Server::worker(void)
{
  try
  {
    zmq::socket_t sock(m_ctx, ZMQ_DEALER);
    sock.connect("inproc://backend");

    while (m_running)
    {
      zmq::message_t identity;
      zmq::message_t msg;
      (void)sock.recv(identity);
      (void)sock.recv(msg);

      // Unpack and convert the RPC name and arguments
      auto data = msgpack::unpack(static_cast<char *>(msg.data()), msg.size());
      std::tuple<std::string, msgpack::object> rpc;
      data.get().convert(rpc);

      // Call the RPC (if present, )
      auto &&name = std::get<0>(rpc);
      auto &&args = std::get<1>(rpc);
      std::unique_ptr<msgpack::v1::object_handle> res;

      if ("terminate" == name)
      {
        // Respond with an empty message
        res = std::make_unique<msgpack::object_handle>();
        reply(sock, identity, res);

        // Now stop the server
        stop();
      }
      else
      {
        if (m_rpcs.find(name) != m_rpcs.end())
        {
          res = m_rpcs.at(name)(args);
        }
        else
        {
          Error err;
          err.m_msg = "'" + name + "' RPC not found!";
          auto zone = std::make_unique<msgpack::zone>();
          auto rtnobj = msgpack::object(err, *zone);
          res =
              std::make_unique<msgpack::object_handle>(rtnobj, std::move(zone));
        }
        reply(sock, identity, res);
      }
    }
  }
  catch (const zmq::error_t &e)
  {
    std::cerr << " !! ZMQ Worker Error " << e.num() << ": " << e.what()
              << std::endl;
  }
}

void Server::reply(zmq::socket_t &sock,
                   zmq::message_t &identity,
                   std::unique_ptr<msgpack::object_handle> &res) const
{
  // Reply with the identity of the message for the broker
  zmq::message_t copied_id;
  copied_id.copy(identity);
  (void)sock.send(copied_id, zmq::send_flags::sndmore);

  // Pack the result into an object
  auto sbuf = std::make_shared<msgpack::sbuffer>();
  msgpack::pack(*sbuf, res->get());
  (void)sock.send(zmq::const_buffer(sbuf->data(), sbuf->size()),
                  zmq::send_flags::none);
}