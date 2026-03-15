#
# Author: Benjamin Sergeant
# Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
#

cmake_minimum_required (VERSION 3.14)
project (ws)

# There's -Weverything too for clang
if (NOT WIN32)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
endif()

set (CMAKE_CXX_STANDARD 11)

option(USE_TLS "Add TLS support" ON)

include_directories(ws .)
include_directories(ws ..)
include_directories(ws ../third_party/cpp-linenoise)
include_directories(ws ../third_party/cli11)
include_directories(ws ../third_party/msgpack11)

include(FetchContent)

add_executable(ws
  ../third_party/msgpack11/msgpack11.cpp
  ../third_party/cpp-linenoise/linenoise.cpp
  ws.cpp)

# library with the most dependencies come first
target_link_libraries(ws ixwebsocket)

target_link_libraries(ws spdlog)

install(TARGETS ws RUNTIME DESTINATION bin)
