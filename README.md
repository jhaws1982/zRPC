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
| msgpack-c  | 4.x+    | MessagePack C++ headers  | N/A (pulled in cmake)        |
| Boost      | 1.71.0+ | Boost Libraries (header) | apt install libboost-all-dev |

## Notes
- Any argument or return value associated with a function must be packable by
  MsgPack, otherwise you will be unable to package the argument properly for
  passing between client and server.

## TODO
- Setup make install in CMake
- Unit testing from the beginning - basic client/server, all ctors, move/copy
  - Write unit test to ensure that all workers are used and client blocks until free worker then continues
- RPC client/server plus generic class/event publisher
  - Easy way to publish generic events as passed in
  - Easy way to subscribe to generic events and receive callback when received (how to include generic data?)
- PlantUML class diagrams

## BUGS
- Determine how to return errors; zRPCError is fine, but how to get either error or value out in single 'as' call?