cmake_minimum_required(VERSION 3.8)
project(snmalloc C CXX)

if (NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type selected, default to: Release")
  set(CMAKE_BUILD_TYPE "Release")
endif()

include(CheckCXXCompilerFlag)
include(CheckCSourceCompiles)

option(USE_SNMALLOC_STATS "Track allocation stats" OFF)
option(SNMALLOC_CI_BUILD "Disable features not sensible for CI" OFF)
option(USE_MEASURE "Measure performance with histograms" OFF)
option(EXPOSE_EXTERNAL_PAGEMAP "Expose the global pagemap" OFF)
option(EXPOSE_EXTERNAL_RESERVE "Expose an interface to reserve memory using the default memory provider" OFF)
option(SNMALLOC_RUST_SUPPORT "Build static library for rust" OFF)
option(SNMALLOC_STATIC_LIBRARY   "Build static libraries" ON)
option(SNMALLOC_QEMU_WORKAROUND "Disable using madvise(DONT_NEED) to zero memory on Linux" Off)
option(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE "Compile for current machine architecture" Off)
set(CACHE_FRIENDLY_OFFSET OFF CACHE STRING "Base offset to place linked-list nodes.")
set(SNMALLOC_STATIC_LIBRARY_PREFIX "sn_" CACHE STRING "Static library function prefix")

# malloc.h will error if you include it on FreeBSD, so this test must not
# unconditionally include it.
CHECK_C_SOURCE_COMPILES("
#if __has_include(<malloc_np.h>)
#include <malloc_np.h>
#if __has_include(<malloc/malloc.h>)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
size_t malloc_usable_size(const void* ptr) { return 0; }
int main() { return 0; }
" CONST_QUALIFIED_MALLOC_USABLE_SIZE)

if ((CMAKE_BUILD_TYPE STREQUAL "Release") AND (NOT SNMALLOC_CI_BUILD))
  option(USE_POSIX_COMMIT_CHECKS "Instrument Posix PAL to check for access to unused blocks of memory." Off)
else ()
  option(USE_POSIX_COMMIT_CHECKS "Instrument Posix PAL to check for access to unused blocks of memory." On)
endif ()

# Provide as macro so other projects can reuse
macro(warnings_high)
  if(MSVC)
    # Force to always compile with W4
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
      string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    else()
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    endif()
    add_compile_options(/WX /wd4127 /wd4324 /wd4201)
  else()
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      add_compile_options(-Wsign-conversion)
    endif ()
    add_compile_options(-Wall -Wextra -Werror -Wundef)
  endif()
endmacro()

macro(oe_simulate target)
  target_compile_definitions(${target} PRIVATE SNMALLOC_USE_SMALL_CHUNKS)
endmacro()

macro(clangformat_targets)
  # The clang-format tool is installed under a variety of different names.  Try
  # to find a sensible one.  Only look for versions 9 explicitly - we don't
  # know whether our clang-format file will work with newer versions of the
  # tool.  It does not work with older versions as AfterCaseLabel is not supported
  # in earlier versions.
  find_program(CLANG_FORMAT NAMES
    clang-format90 clang-format-9)

  # If we've found a clang-format tool, generate a target for it, otherwise emit
  # a warning.
  if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
    message(WARNING "Not generating clangformat target, no clang-format tool found")
  else ()
    message(STATUS "Generating clangformat target using ${CLANG_FORMAT}")
    file(GLOB_RECURSE ALL_SOURCE_FILES *.cc *.h *.hh)
    # clangformat does not yet understand concepts well; for the moment, don't
    # ask it to format them.  See https://reviews.llvm.org/D79773
    list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX "src/pal/pal_concept\.h$")
    add_custom_target(
      clangformat
      COMMAND ${CLANG_FORMAT}
      -i
      ${ALL_SOURCE_FILES})
  endif()
endmacro()

# The main target for snmalloc
add_library(snmalloc_lib INTERFACE)
target_include_directories(snmalloc_lib INTERFACE src/)

if(NOT MSVC)
  find_package(Threads REQUIRED COMPONENTS snmalloc_lib)
  target_link_libraries(snmalloc_lib INTERFACE ${CMAKE_THREAD_LIBS_INIT})
  if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    target_link_libraries(snmalloc_lib INTERFACE atomic)
  endif()
  if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "amd64")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  # XXX elseif ARM?
  endif()
endif()

if (WIN32)
  set(WIN8COMPAT FALSE CACHE BOOL "Avoid Windows 10 APIs")
  if (WIN8COMPAT)
    target_compile_definitions(snmalloc_lib INTERFACE -DWINVER=0x0603)
    message(STATUS "snmalloc: Avoiding Windows 10 APIs")
  else()
    message(STATUS "snmalloc: Using Windows 10 APIs")
    # VirtualAlloc2 is exposed by mincore.lib, not Kernel32.lib (as the
    # documentation says)
    target_link_libraries(snmalloc_lib INTERFACE mincore)
  endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
    target_compile_options(snmalloc_lib INTERFACE -mcx16)
  elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "amd64")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  elseif(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86")
   target_compile_options(snmalloc_lib INTERFACE -mcx16)
  endif()
endif ()

# Have to set this globally, as can't be set on an interface target.
set(CMAKE_CXX_STANDARD 17)

if(USE_SNMALLOC_STATS)
  target_compile_definitions(snmalloc_lib INTERFACE -DUSE_SNMALLOC_STATS)
endif()

if(SNMALLOC_QEMU_WORKAROUND)
  target_compile_definitions(snmalloc_lib INTERFACE -DSNMALLOC_QEMU_WORKAROUND)
endif()

if(USE_MEASURE)
  target_compile_definitions(snmalloc_lib INTERFACE -DUSE_MEASURE)
endif()

if(SNMALLOC_CI_BUILD)
  target_compile_definitions(snmalloc_lib INTERFACE -DSNMALLOC_CI_BUILD)
endif()

if(CACHE_FRIENDLY_OFFSET)
  target_compile_definitions(snmalloc_lib INTERFACE -DCACHE_FRIENDLY_OFFSET=${CACHE_FRIENDLY_OFFSET})
endif()

if(USE_POSIX_COMMIT_CHECKS)
  target_compile_definitions(snmalloc_lib INTERFACE -DUSE_POSIX_COMMIT_CHECKS)
endif()

if(CONST_QUALIFIED_MALLOC_USABLE_SIZE)
  target_compile_definitions(snmalloc_lib INTERFACE -DMALLOC_USABLE_SIZE_QUALIFIER=const)
endif()


# To build with just the header library target define SNMALLOC_ONLY_HEADER_LIBRARY
# in containing Cmake file.
if(NOT DEFINED SNMALLOC_ONLY_HEADER_LIBRARY)

  warnings_high()

  if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
  else()
    add_compile_options(-fno-exceptions -fno-rtti -g -fomit-frame-pointer)
    # Static TLS model is unsupported on Haiku.
    # All symbols are always dynamic on haiku and -rdynamic is redundant (and unsupported).
    if (NOT CMAKE_SYSTEM_NAME MATCHES "Haiku")
	    add_compile_options(-ftls-model=initial-exec)
    	    if(SNMALLOC_CI_BUILD OR (${CMAKE_BUILD_TYPE} MATCHES "Debug"))
      		# Get better stack traces in CI and Debug.
      		target_link_libraries(snmalloc_lib INTERFACE "-rdynamic")
    	    endif()
    endif()

    if(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE)
      check_cxx_compiler_flag(-march=native SUPPORT_MARCH_NATIVE)
      if (SUPPORT_MARCH_NATIVE)
        add_compile_options(-march=native)
      else()
        message(WARNING "Compiler does not support `-march=native` required by SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE")
      endif()
    endif()

    find_package(Backtrace)
    if(${Backtrace_FOUND})
      target_compile_definitions(snmalloc_lib INTERFACE -DBACKTRACE_HEADER="${Backtrace_HEADER}")
      target_link_libraries(snmalloc_lib INTERFACE ${Backtrace_LIBRARIES})
      target_include_directories(snmalloc_lib INTERFACE ${Backtrace_INCLUD_DIRS})
    endif()

  endif()

  macro(subdirlist result curdir)
    file(GLOB children LIST_DIRECTORIES true RELATIVE ${curdir} ${curdir}/*)
    set(dirlist "")
    foreach(child ${children})
      if(IS_DIRECTORY ${curdir}/${child})
        list(APPEND dirlist ${child})
      endif()
    endforeach()
    set(${result} ${dirlist})
  endmacro()

  macro(add_shim name type)
    add_library(${name} ${type} ${ARGN})
    target_link_libraries(${name} snmalloc_lib)
    if(NOT MSVC)
      target_compile_definitions(${name} PRIVATE "SNMALLOC_EXPORT=__attribute__((visibility(\"default\")))")
    endif()
    set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden)

    if(EXPOSE_EXTERNAL_PAGEMAP)
      if(MSVC)
        target_compile_definitions(${name} PRIVATE /DSNMALLOC_EXPOSE_PAGEMAP)
      else()
        target_compile_definitions(${name} PRIVATE -DSNMALLOC_EXPOSE_PAGEMAP)
      endif()
    endif()

    if(EXPOSE_EXTERNAL_RESERVE)
      if(MSVC)
        target_compile_definitions(${name} PRIVATE /DSNMALLOC_EXPOSE_RESERVE)
      else()
        target_compile_definitions(${name} PRIVATE -DSNMALLOC_EXPOSE_RESERVE)
      endif()
    endif()

    # Ensure that we do not link against C++ stdlib when compiling shims.
    if(NOT MSVC)
      set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
    endif()

  endmacro()

  if (SNMALLOC_STATIC_LIBRARY)
    add_shim(snmallocshim-static STATIC src/override/malloc.cc)
    add_shim(snmallocshim-1mib-static STATIC src/override/malloc.cc)
    add_shim(snmallocshim-16mib-static STATIC src/override/malloc.cc)
    target_compile_definitions(snmallocshim-16mib-static PRIVATE SNMALLOC_USE_LARGE_CHUNKS
            SNMALLOC_STATIC_LIBRARY_PREFIX=${SNMALLOC_STATIC_LIBRARY_PREFIX})
    target_compile_definitions(snmallocshim-static PRIVATE
            SNMALLOC_STATIC_LIBRARY_PREFIX=${SNMALLOC_STATIC_LIBRARY_PREFIX})
    target_compile_definitions(snmallocshim-1mib-static PRIVATE
            SNMALLOC_STATIC_LIBRARY_PREFIX=${SNMALLOC_STATIC_LIBRARY_PREFIX})
  endif ()

  if(NOT WIN32)
    set(SHARED_FILES src/override/new.cc src/override/malloc.cc)
    add_shim(snmallocshim SHARED ${SHARED_FILES})
    add_shim(snmallocshim-1mib SHARED ${SHARED_FILES})
    add_shim(snmallocshim-16mib SHARED ${SHARED_FILES})
    target_compile_definitions(snmallocshim-16mib PRIVATE SNMALLOC_USE_LARGE_CHUNKS)
    # Build a shim with some settings from oe.
    add_shim(snmallocshim-oe SHARED ${SHARED_FILES})
    oe_simulate(snmallocshim-oe)
  endif()

  if(SNMALLOC_RUST_SUPPORT)
    add_shim(snmallocshim-rust STATIC src/override/rust.cc)
    add_shim(snmallocshim-1mib-rust STATIC src/override/rust.cc)
    add_shim(snmallocshim-16mib-rust STATIC src/override/rust.cc)
    target_compile_definitions(snmallocshim-16mib-rust PRIVATE SNMALLOC_USE_LARGE_CHUNKS)
  endif()

  enable_testing()

  set(TESTDIR ${CMAKE_CURRENT_SOURCE_DIR}/src/test)
  subdirlist(TEST_CATEGORIES ${TESTDIR})
  list(REVERSE TEST_CATEGORIES)
  foreach(TEST_CATEGORY ${TEST_CATEGORIES})
    subdirlist(TESTS ${TESTDIR}/${TEST_CATEGORY})
    foreach(TEST ${TESTS})
      if (WIN32
          OR (CMAKE_SYSTEM_NAME STREQUAL NetBSD)
          OR (CMAKE_SYSTEM_NAME STREQUAL OpenBSD)
          OR (CMAKE_SYSTEM_NAME STREQUAL DragonFly)
          OR (CMAKE_SYSTEM_NAME STREQUAL SunOS))
        # Windows does not support aligned allocation well enough
        # for pass through.
        # NetBSD, OpenBSD and DragonFlyBSD do not support malloc*size calls.
        set(FLAVOURS 1;16;oe)
      else()
        set(FLAVOURS 1;16;oe;malloc)
      endif()
      foreach(SUPER_SLAB_SIZE ${FLAVOURS})
        unset(SRC)
        aux_source_directory(${TESTDIR}/${TEST_CATEGORY}/${TEST} SRC)
        set(TESTNAME "${TEST_CATEGORY}-${TEST}-${SUPER_SLAB_SIZE}")

        add_executable(${TESTNAME} ${SRC})
        if (${SUPER_SLAB_SIZE} EQUAL 16)
          target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_USE_LARGE_CHUNKS)
        endif()
        if (${SUPER_SLAB_SIZE} STREQUAL "oe")
          oe_simulate(${TESTNAME})
        endif()
        if (${SUPER_SLAB_SIZE} STREQUAL "malloc")
          target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_PASS_THROUGH)
        endif()
        if(CONST_QUALIFIED_MALLOC_USABLE_SIZE)
          target_compile_definitions(${TESTNAME} PRIVATE -DMALLOC_USABLE_SIZE_QUALIFIER=const)
        endif()
        target_link_libraries(${TESTNAME} snmalloc_lib)
        if (${TEST} MATCHES "release-.*")
          message(STATUS "Adding test: ${TESTNAME} only for release configs")
          add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} CONFIGURATIONS "Release")
        else()
          message(STATUS "Adding test: ${TESTNAME}")
          add_test(${TESTNAME} ${TESTNAME})
        endif()
        if (${TEST_CATEGORY} MATCHES "perf")
          message(STATUS "Single threaded test: ${TESTNAME}")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
        endif()
        if(WIN32)
          # On Windows these tests use a lot of memory as it doesn't support
          # lazy commit.
          if (${TEST} MATCHES "two_alloc_types")
            message(STATUS "Single threaded test: ${TESTNAME}")
            set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
          endif()
          if (${TEST} MATCHES "fixed_region")
            message(STATUS "Single threaded test: ${TESTNAME}")
            set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
          endif()
          if (${TEST} MATCHES "memory")
            message(STATUS "Single threaded test: ${TESTNAME}")
            set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
          endif()
        endif()
        if (${TEST_CATEGORY} MATCHES "func")
          target_compile_definitions(${TESTNAME} PRIVATE -DUSE_SNMALLOC_STATS)
        endif ()
      endforeach()
    endforeach()
  endforeach()

  clangformat_targets()
endif()
