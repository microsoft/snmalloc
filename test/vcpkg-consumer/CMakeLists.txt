cmake_minimum_required(VERSION 3.16)
project(snmalloc-vcpkg-test CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_VERBOSE_MAKEFILE ON)

enable_testing()

find_package(snmalloc CONFIG REQUIRED)

add_executable(test-header-only test_header_only.cpp)
target_link_libraries(test-header-only PRIVATE snmalloc::snmalloc)
add_test(NAME header-only COMMAND test-header-only)

if(TARGET snmalloc::snmallocshim-static)
  add_executable(test-static-shim test_static_shim.cpp)
  target_link_libraries(test-static-shim PRIVATE snmalloc::snmallocshim-static)
  add_test(NAME static-shim COMMAND test-static-shim)
endif()
