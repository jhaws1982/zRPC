[![CI](https://github.com/jhaws1982/zRPC/actions/workflows/ci.yml/badge.svg)](https://github.com/jhaws1982/zRPC/actions)
[![License](https://img.shields.io/github/license/jhaws1982/zRPC.svg)](https://github.com/jhaws1982/zRPC/blob/master/LICENSE)

# zRPC
ZeroMQ-based RPC client/server library with MessagePack support.

This was started as a project to fill a need where I wanted an RPC server,
wanted to use C++, and wanted to hide the socket connection details. I also
wanted to utilize a modern socket library that covered more than just IP, and
found ZeroMQ to fit the bill.

In addition, this is a learning project for me to hone my C++ development
skills and delve into some more advanced concepts of C++ such as advanced
templates and scraping function traits.

Some of the ideas, in particular the approach to binding all kinds of functions
in the RPC server, were borrowed from rpclib (https://github.com/rpclib/rpclib).

## Dependencies
| Library    | Version | Description              | Install command (Ubuntu)     |
| ---------- | ------- | ------------------------ | ---------------------------- |
| libzmq     | 4.3.2+  | 0MQ base library         | apt install libzmq5          |
| libzmq-dev | 4.3.2+  | 0MQ development files    | apt install libzmq3-dev      |
| cppzmq     | 4.8+    | 0MQ C++ Header-only API  | N/A (pulled in cmake)        |
| msgpack-c  | 4.x+    | MessagePack C++ headers  | N/A (pulled in cmake)        |
| Boost      | 1.71.0+ | Boost Libraries (header) | apt install libboost-*-dev<br>- chrono<br>- context<br>- filesystem<br>- system<br>- timer<br> |

## Notes
- Any argument or return value associated with a function must be packable by
  MsgPack, otherwise you will be unable to package the argument properly for
  passing between client and server.
- A call either returns the RPC's value or throws, so the value comes out in a
  single `as` call and can never be mistaken for an error:
  ```cpp
  try
  {
    int sum = client.call("add", 2, 3).get().as<int>();
  }
  catch (const zRPC::RemoteError &e)  // the server reported an error
  {
    // e.code(): NotFound, InvalidArguments, Failed (the function threw), ...
  }
  catch (const zRPC::TimeoutError &e) {}   // no reply in time
  catch (const zRPC::ProtocolError &e) {}  // corrupt or incompatible reply
  ```
- Every request, reply and publication is a CRC-32 protected envelope
  `[version, status, payload, crc]`. The CRC detects accidental corruption,
  not tampering; use 0MQ CURVE security on untrusted networks.
- There is no built-in way to stop a server remotely, and no authentication.
  If clients should be able to stop the server, bind an RPC for it yourself:
  `srv.bind("terminate", [&srv]() { srv.stop(); });`

## TODO
- Setup make install in CMake
- Unit testing from the beginning - basic client/server, all ctors, move/copy
  - Add gtest suite as testing dependency
  - Write unit test to ensure that all workers are used and client blocks until free worker then continues
  - Tie into codecov.io
- RPC client/server plus generic class/event publisher
  - Easy way to publish generic events as passed in
  - Easy way to subscribe to generic events and receive callback when received (how to include generic data?)
- PlantUML class diagrams

## BUGS
- None known.