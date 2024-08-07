###############################################################################
# CMakeLists.txt for zRPC library
#  - Creates a CMake target library named 'zRPC'
###############################################################################
cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

project(zRPC
        VERSION "0.0.1"
        DESCRIPTION "0MQ-based RPC client/server library with MessagePack support"
       )

option(ZRPC_BUILD_TESTS "Enable build of unit test applications" ON)

# Setup default compiler flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(compile_options -pedantic-errors
                    -pedantic
                    -Wall
                    -Wextra
                    -Wconversion
                    -Wsign-conversion
                    -Wno-psabi
                    -Werror
    CACHE INTERNAL "Compiler Options"
   )

###############################################################################
# Bring in CPM
###############################################################################
include(cmake/CPM.cmake)

###############################################################################
# Bring in CPPZMQ header-only API
###############################################################################
CPMAddPackage(
  NAME cppzmq
  VERSION 4.8.1
  GITHUB_REPOSITORY "zeromq/cppzmq"
  OPTIONS "CPPZMQ_BUILD_TESTS OFF"
)

###############################################################################
# Bring in MSGPACK-C header-only API
###############################################################################
CPMAddPackage(
  NAME msgpack
  GIT_TAG cpp-4.1.1
  GITHUB_REPOSITORY "msgpack/msgpack-c"
  OPTIONS "MSGPACK_BUILD_DOCS OFF" "MSGPACK_CXX20 ON" "MSGPACK_USE_BOOST OFF"
)

###############################################################################
# Bring in C++ CRC header-only API
###############################################################################
CPMAddPackage(
  NAME CRCpp
  GIT_TAG release-1.1.0.0
  GITHUB_REPOSITORY "d-bahr/CRCpp"
)
if(CRCpp_ADDED)
  add_library(CRCpp INTERFACE)
  target_include_directories(CRCpp SYSTEM INTERFACE ${CRCpp_SOURCE_DIR}/inc)
endif(CRCpp_ADDED)

###############################################################################
# zRPC library
###############################################################################
add_library(${PROJECT_NAME} SHARED)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PUBLIC cppzmq msgpackc-cxx CRCpp pthread)
target_sources(${PROJECT_NAME} PRIVATE  src/zRPCClient.cpp
                                        src/zRPCServer.cpp
                                        src/zRPCPublisher.cpp
                                        src/zRPCSubscriber.cpp
                               PUBLIC   include/zRPC.hpp
              )
target_compile_options(${PROJECT_NAME} PUBLIC ${compile_options})


###############################################################################
# Test applications
###############################################################################
if (ZRPC_BUILD_TESTS)
  add_executable(clientDetached tests/clientDetached.cpp)
  target_link_libraries(clientDetached zRPC)
  target_compile_options(clientDetached PUBLIC ${compile_options})

  add_executable(client tests/client.cpp)
  target_link_libraries(client zRPC)
  target_compile_options(client PUBLIC ${compile_options})

  add_executable(server tests/server.cpp)
  target_link_libraries(server zRPC)
  target_compile_options(server PUBLIC ${compile_options})

  add_executable(publisher tests/publisher.cpp)
  target_link_libraries(publisher zRPC)
  target_compile_options(publisher PUBLIC ${compile_options})

  add_executable(subscriber tests/subscriber.cpp)
  target_link_libraries(subscriber zRPC)
  target_compile_options(subscriber PUBLIC ${compile_options})

  include(cmake/CodeCoverage.cmake)
  append_coverage_compiler_flags()
  add_executable(unittest tests/unit.cpp)
  target_link_libraries(unittest zRPC)
  target_compile_options(unittest PUBLIC ${compile_options})

  setup_target_for_coverage_gcovr_xml(NAME ${PROJECT_NAME}_coverage
                                      EXECUTABLE unittest
                                      DEPENDENCIES unittest
                                      BASE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                                      EXCLUDE "tests"
				                             )
endif(ZRPC_BUILD_TESTS)
