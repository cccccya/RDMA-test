cmake_minimum_required(VERSION 3.0.0)
project(ib_send_after_write VERSION 0.1.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE "Release")
SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g -ggdb -fno-omit-frame-pointer -fno-inline-functions")
# SET(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -O0 -g -ggdb -fsanitize=address -static-libsan")
SET(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3")



add_executable(RCServer RCServer.cpp rdma.cpp)
target_link_libraries(RCServer
  ibverbs
)

add_executable(RCClient RCClient.cpp rdma.cpp)
target_link_libraries(RCClient
  ibverbs
)