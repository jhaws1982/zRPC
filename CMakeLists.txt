cmake_minimum_required(VERSION 3.21)

project(zRPC)
include(cmake/cppzmq.cmake)



# find_package(cppzmq)
add_library(${PROJECT_NAME} INTERFACE)
target_link_libraries(${PROJECT_NAME} INTERFACE cppzmq)


add_executable(client tests/client.cpp)
target_link_libraries(client zRPC)
add_executable(server tests/server.cpp)
target_link_libraries(server zRPC)