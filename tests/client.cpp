//
//  Hello World client in C++
//  Connects REQ socket to tcp://localhost:5555
//  Sends "Hello" to server, expects "World" back
//
#include <iostream>
#include <string>
#include <zmq.hpp>

int main()
{
  int major, minor, patch;
  zmq_version(&major, &minor, &patch);
  printf("Installed ZeroMQ version: %d.%d.%d\n", major, minor, patch);

  //  Prepare our context and socket
  zmq::context_t context(1);
  zmq::socket_t socket(context, ZMQ_REQ);

  std::cout << "Connecting to hello world server…" << std::endl;
  socket.connect("tcp://localhost:5555");

  //  Do 10 requests, waiting each time for a response
  for (int request_nbr = 0; request_nbr != 10; request_nbr++)
  {
    zmq::message_t request(6);
    memcpy((void *)request.data(), "Hello", 5);
    std::cout << "Sending Hello " << request_nbr << "…" << std::endl;
    socket.send(request, zmq::send_flags::none);

    //  Get the reply.
    zmq::message_t reply;
    socket.recv(reply, zmq::recv_flags::none);
    std::cout << "Received World " << request_nbr << std::endl;
  }
  return 0;
}