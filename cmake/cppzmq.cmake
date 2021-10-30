include(FetchContent)
set(CPPZMQ_BUILD_TESTS OFF CACHE INTERNAL "") # Disable CPPZMQ tests
FetchContent_Declare(cppzmq_git
  GIT_REPOSITORY      https://github.com/zeromq/cppzmq.git
  GIT_TAG             v4.8.1
)
FetchContent_MakeAvailable(cppzmq_git)