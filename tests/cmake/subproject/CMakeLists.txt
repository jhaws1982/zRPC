###############################################################################
# Configure-only check, run by ctest: a project that pulls zRPC in with
# add_subdirectory() keeps CMake's default library type for its own
# libraries, and gets a static zRPC unless it asks otherwise.
###############################################################################
cmake_minimum_required(VERSION 3.16...3.31)
project(zrpc_subproject_check LANGUAGES CXX)

add_subdirectory(${ZRPC_SOURCE_DIR} zrpc)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/parent.cpp "int parent() { return 1; }\n")
add_library(parent ${CMAKE_CURRENT_BINARY_DIR}/parent.cpp)

get_target_property(parent_type parent TYPE)
if(NOT parent_type STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR
          "zRPC changed the parent project's default library type to ${parent_type}")
endif()

get_target_property(zrpc_type zRPC TYPE)
if(NOT zrpc_type STREQUAL "STATIC_LIBRARY")
  message(FATAL_ERROR
          "zRPC as a subproject should default to a static library, not ${zrpc_type}")
endif()

message(STATUS "zRPC subproject check passed")
