message(INFO " -> Fetching cppzmq...")
include(FetchContent)
FetchContent_Declare(cppzmq_git
  GIT_REPOSITORY      https://github.com/zeromq/cppzmq.git
  GIT_TAG             v4.8.1
)
FetchContent_MakeAvailable(cppzmq_git)