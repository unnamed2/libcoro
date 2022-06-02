cmake_minimum_required(VERSION 3.12)
project(libcoro)

set(CMAKE_CONFIGURATION_TYPES "Debug;Release")
set(CMAKE_CXX_STANDARD 20)

include_directories(include)

file(GLOB SRCS src/*.cpp)
file(GLOB HEADERS include/*.h)

add_executable(coroutine_test test/coroutine_test.cpp ${SRCS} ${HEADERS})

add_executable(network_test test/network_test.cpp ${SRCS} ${HEADERS})