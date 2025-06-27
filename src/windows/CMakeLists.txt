cmake_minimum_required(VERSION 3.14)
include(FetchContent)

FetchContent_Declare(
  vcpkg
  GIT_REPOSITORY https://github.com/microsoft/vcpkg
  GIT_TAG        2025.06.13
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(vcpkg)

set(CMAKE_TOOLCHAIN_FILE
            ${vcpkg_SOURCE_DIR}/scripts/buildsystems/vcpkg.cmake
            CACHE STRING "vcpkg toolchain file")

project(snmallocdetours CXX)

add_subdirectory(../.. src/snmalloc EXCLUDE_FROM_ALL)

# On Windows, we need to use the detours library to override the malloc
# functions.
find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")
find_library(DETOURS_LIBRARY detours REQUIRED)

add_library(snmallocdetours OBJECT override/detours.cc)

target_include_directories(snmallocdetours PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(snmallocdetours PUBLIC snmalloc)
target_link_libraries(snmallocdetours PRIVATE ${DETOURS_LIBRARY})

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_library(snmallocdetourslib SHARED override/lib.cc)
target_include_directories(snmallocdetourslib PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(snmallocdetourslib PRIVATE snmallocdetours)

add_executable(snmallocdetoursexample test/main.cc)
target_link_libraries(snmallocdetoursexample snmallocdetourslib)
# Check if 32 bit windows.
if (CMAKE_SIZEOF_VOID_P EQUAL 4)
  target_link_options(snmallocdetoursexample PRIVATE "/INCLUDE:_is_snmalloc_detour")
else()
  target_link_options(snmallocdetoursexample PRIVATE "/INCLUDE:is_snmalloc_detour")
endif()

add_test(NAME snmallocdetours COMMAND snmallocdetoursexample)