cmake_minimum_required(VERSION 3.14)
project(snmalloc CXX)

if (NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type selected, default to: Release")
  set(CMAKE_BUILD_TYPE "Release")
endif()

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

option(USE_SNMALLOC_STATS "Track allocation stats" OFF)
option(SNMALLOC_CI_BUILD "Disable features not sensible for CI" OFF)
option(EXPOSE_EXTERNAL_PAGEMAP "Expose the global pagemap" OFF)
option(EXPOSE_EXTERNAL_RESERVE "Expose an interface to reserve memory using the default memory provider" OFF)
option(SNMALLOC_RUST_SUPPORT "Build static library for rust" OFF)
option(SNMALLOC_STATIC_LIBRARY   "Build static libraries" ON)
option(SNMALLOC_QEMU_WORKAROUND "Disable using madvise(DONT_NEED) to zero memory on Linux" Off)
option(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE "Compile for current machine architecture" Off)
set(SNMALLOC_STATIC_LIBRARY_PREFIX "sn_" CACHE STRING "Static library function prefix")
option(SNMALLOC_HEADER_ONLY_LIBRARY "Use snmalloc has a header-only library" OFF)
option(SNMALLOC_USE_CXX17 "Build as C++17 for legacy support." OFF)
option(SNMALLOC_USE_PTHREAD_DESTRUCTORS "Build using pthread destructors. Reduces the system dependencies, but may interact badly with C++ on some platforms." OFF)

if (NOT MSVC)
  # If CheckLinkerFlag doesn't exist then provide a dummy implementation that
  # always fails.  The fallback can be removed when we move to CMake 3.18 as the
  # baseline.
  include(CheckLinkerFlag OPTIONAL RESULT_VARIABLE CHECK_LINKER_FLAG)
  if (NOT CHECK_LINKER_FLAG)
    function(check_linker_flag)
    endfunction()
  endif ()
  # If the target compiler doesn't support -nostdlib++ then we must enable C at
  # the global scope for the fallbacks to work.
  check_linker_flag(CXX "-nostdlib++" SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX)
  if (NOT SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX AND NOT SNMALLOC_HEADER_ONLY_LIBRARY)
    enable_language(C)
  endif()
endif()

# Define a generator expression for things that will be enabled in either CI
# builds or debug mode.
set(ci_or_debug "$<OR:$<BOOL:${SNMALLOC_CI_BUILD}>,$<CONFIG:Debug>>")

# malloc.h will error if you include it on FreeBSD, so this test must not
# unconditionally include it.
CHECK_CXX_SOURCE_COMPILES("
#if __has_include(<malloc_np.h>)
#include <malloc_np.h>
#endif
#if __has_include(<malloc/malloc.h>)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
size_t malloc_usable_size(const void* ptr) { return 0; }
int main() { return 0; }
" CONST_QUALIFIED_MALLOC_USABLE_SIZE)

# Some libcs might not have getentropy, e.g. it appeared in glibc 2.25
# so we need to fallback if we cannot compile this
CHECK_CXX_SOURCE_COMPILES("
#if __has_include(<unistd.h>)
#  include <unistd.h>
#endif
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif
int main() {
  int entropy = 0;
  int res = getentropy(&entropy, sizeof(entropy));
  return res;
}
" SNMALLOC_PLATFORM_HAS_GETENTROPY)

# Provide as function so other projects can reuse
# FIXME: This modifies some variables that may or may not be the ones that
# provide flags and so is broken by design.  It should be removed once Verona
# no longer uses it.
function(warnings_high)
  if(MSVC)
    # Force to always compile with W4
    if(CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
      string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    else()
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
    endif()
    # /Wv18 is required for the annotation to force inline a lambda.
    add_compile_options(/WX /wd4127 /wd4324 /wd4201)
  else()
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      add_compile_options(-Wsign-conversion -Wconversion)
    endif ()
    add_compile_options(-Wall -Wextra -Werror -Wundef)
  endif()
endfunction()

function(clangformat_targets)
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
    file(GLOB_RECURSE ALL_SOURCE_FILES CONFIGURE_DEPENDS src/*.cc src/*.h src/*.hh)
    # clangformat does not yet understand concepts well; for the moment, don't
    # ask it to format them.  See https://reviews.llvm.org/D79773
    list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX "src/[^/]*/[^/]*_concept\.h$")
    add_custom_target(
      clangformat
      COMMAND ${CLANG_FORMAT}
      -i
      ${ALL_SOURCE_FILES})
  endif()
endfunction()

# The main target for snmalloc.  This is the exported target for the
# header-only configuration and is used as a dependency for all of the builds
# that compile anything.
add_library(snmalloc INTERFACE)

if(SNMALLOC_USE_CXX17)
  target_compile_definitions(snmalloc INTERFACE -DSNMALLOC_USE_CXX17)
  target_compile_features(snmalloc INTERFACE cxx_std_17)
else()
  target_compile_features(snmalloc INTERFACE cxx_std_20)
endif()

# Add header paths.
target_include_directories(snmalloc
  INTERFACE
    $<INSTALL_INTERFACE:include/snmalloc>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

if(NOT MSVC)
  find_package(Threads REQUIRED COMPONENTS snmalloc)
  target_link_libraries(snmalloc INTERFACE
    ${CMAKE_THREAD_LIBS_INIT} $<$<CXX_COMPILER_ID:GNU>:atomic>)
endif()

if (WIN32)
  set(WIN8COMPAT FALSE CACHE BOOL "Avoid Windows 10 APIs")
  target_compile_definitions(snmalloc INTERFACE $<$<BOOL:${WIN8COMPAT}:WINVER=0x0603>)
  # VirtualAlloc2 is exposed by mincore.lib, not Kernel32.lib (as the
  # documentation says)
  target_link_libraries(snmalloc INTERFACE $<$<NOT:$<BOOL:${WIN8COMPAT}>>:mincore>)
  message(STATUS "snmalloc: Avoiding Windows 10 APIs is ${WIN8COMPAT}")
endif()

# Detect support for cmpxchg16b; Werror is needed to make sure mcx16 must be used by targets
check_cxx_compiler_flag("-Werror -Wextra -Wall -mcx16" SNMALLOC_COMPILER_SUPPORT_MCX16)
if(SNMALLOC_COMPILER_SUPPORT_MCX16)
  target_compile_options(snmalloc INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-mcx16>)
endif()

# Helper function that conditionally defines a macro for the build target if
# the CMake variable of the same name is set.
function(add_as_define FLAG)
  target_compile_definitions(snmalloc INTERFACE $<$<BOOL:${${FLAG}}>:${FLAG}>)
endfunction()

add_as_define(USE_SNMALLOC_STATS)
add_as_define(SNMALLOC_QEMU_WORKAROUND)
add_as_define(SNMALLOC_CI_BUILD)
add_as_define(SNMALLOC_PLATFORM_HAS_GETENTROPY)
add_as_define(SNMALLOC_USE_PTHREAD_DESTRUCTORS)
target_compile_definitions(snmalloc INTERFACE $<$<BOOL:CONST_QUALIFIED_MALLOC_USABLE_SIZE>:MALLOC_USABLE_SIZE_QUALIFIER=const>)

# In debug and CI builds, link the backtrace library so that we can get stack
# traces on errors.
find_package(Backtrace)
if(${Backtrace_FOUND})
  target_compile_definitions(snmalloc INTERFACE
    $<${ci_or_debug}:SNMALLOC_BACKTRACE_HEADER="${Backtrace_HEADER}">)
  target_link_libraries(snmalloc INTERFACE
    $<${ci_or_debug}:${Backtrace_LIBRARIES}>)
  target_include_directories(snmalloc INTERFACE
    $<${ci_or_debug}:${Backtrace_INCLUDE_DIRS}>)
endif()

if(MSVC)
  target_compile_definitions(snmalloc INTERFACE -D_HAS_EXCEPTIONS=0)
else()
  if (NOT CMAKE_SYSTEM_NAME MATCHES "Haiku")
    # Get better stack traces in CI and debug builds.
    target_link_options(snmalloc INTERFACE $<${ci_or_debug}:-rdynamic>)
  endif()
endif()


# To build with just the header library target define SNMALLOC_HEADER_ONLY_LIBRARY
if(NOT SNMALLOC_HEADER_ONLY_LIBRARY)

  function(subdirlist result curdir)
    file(GLOB children CONFIGURE_DEPENDS LIST_DIRECTORIES true RELATIVE ${curdir} ${curdir}/* )
    set(dirlist "")
    foreach(child ${children})
      if(IS_DIRECTORY ${curdir}/${child})
        list(APPEND dirlist ${child})
      endif()
    endforeach()
    set(${result} ${dirlist} PARENT_SCOPE)
  endfunction()

  set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld)
  check_cxx_source_compiles("int main() { return 1; }" LLD_WORKS)
  if (LLD_WORKS)
    message(STATUS "Using LLD to link snmalloc shims")
  endif()

  function(add_shim name type)
    add_library(${name} ${type} ${ARGN})
    target_link_libraries(${name} snmalloc)
    set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden)

    if(MSVC)
      target_compile_options(${name} /Zi /W4 /WX /wd4127 /wd4324 /wd4201)
      set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
    else()
      target_compile_definitions(${name} PRIVATE "SNMALLOC_EXPORT=__attribute__((visibility(\"default\")))")
      target_compile_options(${name} PRIVATE
        -fno-exceptions -fno-rtti -fomit-frame-pointer -ffunction-sections
        -Wall -Wextra -Werror -Wundef
        $<$<CXX_COMPILER_ID:Clang>:-Wsign-conversion -Wconversion>)
      # Static TLS model is unsupported on Haiku.
      # All symbols are always dynamic on haiku and -rdynamic is redundant (and unsupported).
      if (NOT CMAKE_SYSTEM_NAME MATCHES "Haiku")
        target_compile_options(${name} PRIVATE -ftls-model=initial-exec)
        target_compile_options(${name} PRIVATE $<$<BOOL:${SNMALLOC_CI_BUILD}>:-g>)
      endif()

      if(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE)
        check_cxx_compiler_flag(-march=native SUPPORT_MARCH_NATIVE)
        if (SUPPORT_MARCH_NATIVE)
          target_compile_options(${name} -march=native)
        else()
          message(WARNING "Compiler does not support `-march=native` required by SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE")
        endif()
      endif()

      # Ensure that we do not link against C++ stdlib when compiling shims.
      # If the compiler supports excluding the C++ stdlib implementation, use
      # it.  Otherwise, fall back to linking the library as if it were C, which
      # has roughly the same effect.
      check_linker_flag(CXX "-nostdlib++" SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX)
      if (SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX)
        target_link_options(${name} PRIVATE -nostdlib++)
      else()
        set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
      endif()

        # Only include the dependencies that actually are touched.
        # Required so C++ library is not included with direct pthread access.
        target_link_options(${name} PRIVATE $<$<BOOL:${LLD_WORKS}>:-Wl,--as-needed>)
        # Remove all the duplicate new/malloc and free/delete definitions
        target_link_options(${name} PRIVATE $<$<BOOL:${LLD_WORKS}>:-Wl,--icf=all -fuse-ld=lld>)
    endif()

    install(TARGETS ${name} EXPORT snmallocConfig)

  endfunction()

  if (SNMALLOC_STATIC_LIBRARY)
    add_shim(snmallocshim-static STATIC src/override/new.cc)
    target_compile_definitions(snmallocshim-static PRIVATE
            SNMALLOC_STATIC_LIBRARY_PREFIX=${SNMALLOC_STATIC_LIBRARY_PREFIX})
  endif ()

  if(NOT WIN32)
    set(SHARED_FILES src/override/new.cc)
    add_shim(snmallocshim SHARED ${SHARED_FILES})
    add_shim(snmallocshim-checks SHARED ${SHARED_FILES})
    target_compile_definitions(snmallocshim-checks PRIVATE SNMALLOC_CHECK_CLIENT)
  endif()

  if(SNMALLOC_RUST_SUPPORT)
    add_shim(snmallocshim-rust STATIC src/override/rust.cc)
    add_shim(snmallocshim-checks-rust STATIC src/override/rust.cc)
    target_compile_definitions(snmallocshim-checks-rust PRIVATE SNMALLOC_CHECK_CLIENT)
  endif()

  enable_testing()

  set(TESTDIR ${CMAKE_CURRENT_SOURCE_DIR}/src/test)
  subdirlist(TEST_CATEGORIES ${TESTDIR})
  list(REVERSE TEST_CATEGORIES)
  foreach(TEST_CATEGORY ${TEST_CATEGORIES})
    message(STATUS "Adding ${TEST_CATEGORY} tests")
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
        set(FLAVOURS fast;check)
      else()
        set(FLAVOURS fast;check;malloc)
      endif()
      foreach(FLAVOUR ${FLAVOURS})
        unset(SRC)
        aux_source_directory(${TESTDIR}/${TEST_CATEGORY}/${TEST} SRC)
        set(TESTNAME "${TEST_CATEGORY}-${TEST}-${FLAVOUR}")

        add_executable(${TESTNAME} ${SRC})

        if (${FLAVOUR} STREQUAL "malloc")
          target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_PASS_THROUGH)
        endif()
        if (${FLAVOUR} STREQUAL "check")
          target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_CHECK_CLIENT)
        endif()
        target_link_libraries(${TESTNAME} snmalloc)
        if (${TEST} MATCHES "release-.*")
          message(VERBOSE "Adding test: ${TESTNAME} only for release configs")
          add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} CONFIGURATIONS "Release")
        else()
          message(VERBOSE "Adding test: ${TESTNAME}")
          add_test(${TESTNAME} ${TESTNAME})
        endif()
        if (${TEST_CATEGORY} MATCHES "perf")
          message(VERBOSE "Single threaded test: ${TESTNAME}")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
        endif()
        if(WIN32)
          # On Windows these tests use a lot of memory as it doesn't support
          # lazy commit.
          if (${TEST} MATCHES "two_alloc_types")
            message(VERBOSE "Single threaded test: ${TESTNAME}")
            set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
          endif()
          if (${TEST} MATCHES "fixed_region")
            message(VERBOSE "Single threaded test: ${TESTNAME}")
            set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
          endif()
          if (${TEST} MATCHES "memory")
            message(VERBOSE "Single threaded test: ${TESTNAME}")
            set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
          endif()
        endif()
        # if (${TEST_CATEGORY} MATCHES "func")
        #   target_compile_definitions(${TESTNAME} PRIVATE -DUSE_SNMALLOC_STATS)
        # endif ()
      endforeach()
    endforeach()
  endforeach()

  clangformat_targets()
endif()

install(TARGETS snmalloc EXPORT snmallocConfig)

install(TARGETS EXPORT snmallocConfig DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/snmalloc)

install(DIRECTORY src/aal DESTINATION include/snmalloc)
install(DIRECTORY src/ds DESTINATION include/snmalloc)
install(DIRECTORY src/override DESTINATION include/snmalloc)
install(DIRECTORY src/backend DESTINATION include/snmalloc)
install(DIRECTORY src/mem DESTINATION include/snmalloc)
install(DIRECTORY src/pal DESTINATION include/snmalloc)
install(FILES src/snmalloc.h;src/snmalloc_core.h;src/snmalloc_front.h DESTINATION include/snmalloc)

install(EXPORT snmallocConfig
  FILE snmalloc-config.cmake
  NAMESPACE snmalloc::
  DESTINATION "share/snmalloc"
)

