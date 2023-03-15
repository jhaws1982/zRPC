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
    Server("tcp://*:" + std::to_string(port), nWorkers)
{
}

Server::Server(const std::string &uri,
               const uint32_t nWorkers) :
    m_ctx(16),
    m_brokerFrontend(m_ctx, zmq::socket_type::router),
    m_brokerBackend(m_ctx, zmq::socket_type::dealer),
    m_crcTable(CRC::CRC_32())
{
  try
  {
    // Start the broker sockets and bind to their ports
    m_brokerFrontend.bind(uri);
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
    zmq::socket_t sock(m_ctx, zmq::socket_type::dealer);
    sock.connect("inproc://backend");

    while (m_running)
    {
      zmq::message_t identity;
      zmq::message_t msg;
      (void)sock.recv(identity);
      (void)sock.recv(msg);

      // Unpack and convert the message and CRC
      auto crcdata =
          msgpack::unpack(static_cast<char *>(msg.data()), msg.size());
      std::tuple<std::string, std::uint32_t> crcrpc;
      crcdata.get().convert(crcrpc);

      auto &&rpcmsg = std::get<0>(crcrpc);
      auto &&crc = std::get<1>(crcrpc);
      std::uint32_t check =
          CRC::Calculate(rpcmsg.data(), rpcmsg.size(), m_crcTable);

      std::unique_ptr<ReturnType> res;
      if (check == crc)
      {
        // Unpack and convert RPC name and arguments
        std::tuple<std::string, msgpack::object> rpc;
        auto data =
            msgpack::unpack(static_cast<char *>(rpcmsg.data()), rpcmsg.size());
        data.get().convert(rpc);

        // Call the RPC
        auto &&name = std::get<0>(rpc);
        auto &&args = std::get<1>(rpc);

        if ("terminate" == name)
        {
          // Respond with an empty message
          res = std::make_unique<ReturnType>();
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
            auto hdl = msgpack::object_handle(rtnobj, std::move(zone));
            std::uint32_t crc = CRC::Calculate(
                hdl.get().via.bin.ptr, hdl.get().via.bin.size, m_crcTable);
            res = std::make_unique<ReturnType>(
                std::make_tuple(std::move(hdl.get()), std::move(crc)));
          }
          reply(sock, identity, res);
        }
      }
      else
      {
        Error err;
        std::stringstream ss;
        ss << std::hex << "Bad checksum: CRC=" << crc << " != " << check
           << "=Checked";
        std::cout << ss.str() << std::endl;
        err.m_msg = ss.str();
        auto zone = std::make_unique<msgpack::zone>();
        auto rtnobj = msgpack::object(err, *zone);
        auto hdl = msgpack::object_handle(rtnobj, std::move(zone));
        std::uint32_t crc = CRC::Calculate(hdl.get().via.bin.ptr,
                                           hdl.get().via.bin.size, m_crcTable);
        res = std::make_unique<ReturnType>(
            std::make_tuple(std::move(hdl.get()), std::move(crc)));
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
                   std::unique_ptr<ReturnType> &res) const
{
  // Reply with the identity of the message for the broker
  zmq::message_t copied_id;
  copied_id.copy(identity);
  (void)sock.send(copied_id, zmq::send_flags::sndmore);

  // Pack the result into an object
  auto sbuf = std::make_shared<msgpack::sbuffer>();
  msgpack::pack(*sbuf, *res);
  (void)sock.send(zmq::const_buffer(sbuf->data(), sbuf->size()),
                  zmq::send_flags::none);
}