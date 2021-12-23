include(FetchContent)
set(MSGPACK_BUILD_DOCS OFF CACHE INTERNAL "") # Disable Doxygen docs
set(MSGPACK_CXX20 ON CACHE INTERNAL "") # Enable C++ 20
FetchContent_Declare(msgpack-c_git
  GIT_REPOSITORY      https://github.com/msgpack/msgpack-c
  GIT_TAG             cpp-4.0.3
)
FetchContent_MakeAvailable(msgpack-c_git)
