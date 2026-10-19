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
| Library     | Version | Description              | Package (Debian/Ubuntu) |
| ----------- | ------- | ------------------------ | ----------------------- |
| libzmq      | 4.3.2+  | 0MQ base library         | `libzmq3-dev`           |
| cppzmq      | 4.8.1+  | 0MQ C++ header-only API  | `cppzmq-dev`            |
| msgpack-cxx | 6.0+    | MessagePack C++ headers  | `libmsgpack-cxx-dev`    |

A C++20 compiler and CMake 3.16+ are required. libzmq must be installed.
cppzmq and msgpack-cxx are used from the system when found; otherwise CMake
downloads them (cppzmq 4.10.0, msgpack-cxx 7.0.0) unless
`-DZRPC_FETCH_DEPS=OFF`. zRPC does not use Boost, although a distribution's
msgpack-cxx package may pull it in.

## Building
```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

| Option                    | Default     | Effect                                                     |
| ------------------------- | ----------- | ---------------------------------------------------------- |
| `ZRPC_FETCH_DEPS`         | `ON`        | Download cppzmq/msgpack-cxx with CPM when not installed     |
| `ZRPC_BUILD_TESTS`        | top level   | Build the tests and examples                               |
| `ZRPC_INSTALL`            | top level   | Generate install and CPack rules                           |
| `ZRPC_WARNINGS_AS_ERRORS` | top level   | Add `-Werror` to zRPC's own targets (never to consumers)   |
| `ZRPC_ENABLE_COVERAGE`    | `OFF`       | Instrument for coverage; adds the `zRPC_coverage` target   |
| `BUILD_SHARED_LIBS`       | `ON`        | Build a shared (`ON`) or static (`OFF`) library            |
| `ZRPC_PACKAGE_ARCH`       | build host  | Package architecture for cross builds, e.g. `arm64`        |

## Using zRPC in another project
After `cmake --install build` (or installing the `libzrpc-dev` package):
```cmake
find_package(zRPC 0.1 REQUIRED)
target_link_libraries(app PRIVATE zRPC::zRPC)
```
and `#include <zRPC.hpp>`. `add_subdirectory()` and `CPMAddPackage()` work
too; tests, install rules and `-Werror` are then off by default.

## Packaging
CPack builds a runtime package (`libzrpc<major>.<minor>`: the shared library)
and a development package (`libzrpc-dev`/`libzrpc-devel`: headers and the CMake
package) as DEB, RPM (when `rpmbuild` is installed) and TGZ:
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DZRPC_FETCH_DEPS=OFF -DZRPC_BUILD_TESTS=OFF
cmake --build build
cd build && cpack
```
Distribution and cross builds should use `-DZRPC_FETCH_DEPS=OFF` so that every
dependency comes from the target's sysroot. For a cross build, pass your
toolchain file and the target's package architecture, e.g.
`-DCMAKE_TOOLCHAIN_FILE=aarch64.cmake -DZRPC_PACKAGE_ARCH=arm64`. CI builds amd64
and arm64 packages for every release.

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