# Host tests for include/calium_core.hpp. Needs no Geode SDK and no OpenGL, only a C++20
# compiler, so CI can run them on a cheap ubuntu runner in seconds:
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure

cmake_minimum_required(VERSION 3.21)
project(CaliumCoreTests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(core_test core_test.cpp)
target_include_directories(core_test PRIVATE ../include)
if (NOT MSVC)
  target_compile_options(core_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME calium_core COMMAND core_test)
