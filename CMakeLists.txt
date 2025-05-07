cmake_minimum_required(VERSION 3.14)
project(snmalloc CXX)

if (NOT CMAKE_BUILD_TYPE)
  message(STATUS "No build type selected, default to: Release")
  set(CMAKE_BUILD_TYPE "Release")
endif()

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CheckIPOSupported)
include(CMakeDependentOption)

# Name chosen for compatibility with CTest.
option(SNMALLOC_BUILD_TESTING "Build test programs as well as shims" ON)

option(SNMALLOC_HEADER_ONLY_LIBRARY "Use snmalloc has a header-only library" OFF)
# Options that apply globally
option(SNMALLOC_CI_BUILD "Disable features not sensible for CI" OFF)
option(SNMALLOC_QEMU_WORKAROUND "Disable using madvise(DONT_NEED) to zero memory on Linux" Off)
option(SNMALLOC_USE_CXX17 "Build as C++17 for legacy support." OFF)
option(SNMALLOC_TRACING "Enable large quantities of debug output." OFF)
option(SNMALLOC_NO_REALLOCARRAY "Build without reallocarray exported" ON)
option(SNMALLOC_NO_REALLOCARR "Build without reallocarr exported" ON)
option(SNMALLOC_LINK_ICF "Link with Identical Code Folding" ON)
option(SNMALLOC_IPO "Link with IPO/LTO support" OFF)
option(SNMALLOC_BENCHMARK_INDIVIDUAL_MITIGATIONS "Build tests and ld_preload for individual mitigations" OFF)
option(SNMALLOC_ENABLE_DYNAMIC_LOADING "Build such that snmalloc can be dynamically loaded. This is not required for LD_PRELOAD, and will harm performance if enabled." OFF)
option(SNMALLOC_ENABLE_WAIT_ON_ADDRESS "Use wait on address backoff strategy if it is available" ON)
option(SNMALLOC_ENABLE_FUZZING "Enable fuzzing instrumentation tests" OFF)
option(SNMALLOC_USE_SELF_VENDORED_STL "Avoid using system STL" OFF)
# Options that apply only if we're not building the header-only library
cmake_dependent_option(SNMALLOC_RUST_SUPPORT "Build static library for rust" OFF "NOT SNMALLOC_HEADER_ONLY_LIBRARY" OFF)
cmake_dependent_option(SNMALLOC_RUST_LIBC_API "Include libc API in the rust library" OFF "SNMALLOC_RUST_SUPPORT" OFF)
cmake_dependent_option(SNMALLOC_STATIC_LIBRARY "Build static libraries" ON "NOT SNMALLOC_HEADER_ONLY_LIBRARY" OFF)
cmake_dependent_option(SNMALLOC_CHECK_LOADS "Perform bounds checks on the source argument to memcpy with heap objects" OFF "NOT SNMALLOC_HEADER_ONLY_LIBRARY" OFF)
cmake_dependent_option(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE "Compile for current machine architecture" Off "NOT SNMALLOC_HEADER_ONLY_LIBRARY" OFF)
cmake_dependent_option(SNMALLOC_PAGEID "Set an id to memory regions" OFF "NOT SNMALLOC_PAGEID" OFF)

# GwpAsan secondary allocator
option(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION "Enable GwpAsan as a secondary allocator" OFF)
set(SNMALLOC_GWP_ASAN_INCLUDE_PATH "" CACHE PATH "GwpAsan header directory")
set(SNMALLOC_GWP_ASAN_LIBRARY_PATH "" CACHE PATH "GwpAsan library directory")

if (NOT SNMALLOC_HEADER_ONLY_LIBRARY)
  # Pick a sensible default for the thread cleanup mechanism
  if (${CMAKE_SYSTEM_NAME} STREQUAL FreeBSD)
    set(SNMALLOC_CLEANUP_DEFAULT THREAD_CLEANUP)
  elseif (UNIX AND NOT APPLE)
    set(SNMALLOC_CLEANUP_DEFAULT CXX11_THREAD_ATEXIT_DIRECT)
  else ()
    set(SNMALLOC_CLEANUP_DEFAULT CXX11_DESTRUCTORS)
  endif()
  # Specify the thread cleanup mechanism to use.
  set(SNMALLOC_CLEANUP ${SNMALLOC_CLEANUP_DEFAULT} CACHE STRING "The mechanism that snmalloc will use for thread destructors.  Valid options are: CXX11_DESTRUCTORS (use C++11 destructors, may depend on the C++ runtime library), CXX11_THREAD_ATEXIT_DIRECT (use the glibc call for libstdc++ directly), PTHREAD_DESTRUCTORS (use pthreads, may interact badly with C++ on some platforms, such as macOS) THREAD_CLEANUP (depend on an explicit call to _malloc_thread_cleanup on thread exit, supported by FreeBSD's threading implementation and possibly elsewhere)")
  set_property(CACHE SNMALLOC_CLEANUP PROPERTY STRINGS THREAD_CLEANUP PTHREAD_DESTRUCTORS CXX11_THREAD_ATEXIT_DIRECT CXX11_DESTRUCTORS)

  set(SNMALLOC_STATIC_LIBRARY_PREFIX "sn_" CACHE STRING "Static library function prefix")
  set(SNMALLOC_COMPILER_SUPPORT_IPO FALSE)
else ()
  unset(SNMALLOC_STATIC_LIBRARY_PREFIX CACHE)
  unset(SNMALLOC_CLEANUP CACHE)
endif ()

if (NOT SNMALLOC_CLEANUP STREQUAL CXX11_DESTRUCTORS)
  set(CMAKE_CXX_IMPLICIT_LINK_LIBRARIES "")
endif()

set(SNMALLOC_SANITIZER "" CACHE STRING "Use sanitizer type (undefined|thread|...)")
if (SNMALLOC_SANITIZER)
  message(STATUS "Using sanitizer=${SNMALLOC_SANITIZER}")
endif()

set(SNMALLOC_MIN_ALLOC_SIZE "" CACHE STRING "Minimum allocation bytes (power of 2)")
set(SNMALLOC_MIN_ALLOC_STEP_SIZE "" CACHE STRING "Minimum allocation step (power of 2)")

set(SNMALLOC_PAGESIZE "" CACHE STRING "Page size in bytes")

set(SNMALLOC_DEALLOC_BATCH_RING_ASSOC "" CACHE STRING "Associativity of deallocation batch cache; 0 to disable")
set(SNMALLOC_DEALLOC_BATCH_RING_SET_BITS "" CACHE STRING "Logarithm of number of deallocation batch cache associativity sets")

if(MSVC AND SNMALLOC_STATIC_LIBRARY AND (SNMALLOC_STATIC_LIBRARY_PREFIX STREQUAL ""))
  message(FATAL_ERROR "Empty static library prefix not supported on MSVC")
endif()

# If CheckLinkerFlag doesn't exist then provide a dummy implementation that
# always fails.  The fallback can be removed when we move to CMake 3.18 as the
# baseline.
include(CheckLinkerFlag OPTIONAL RESULT_VARIABLE CHECK_LINKER_FLAG)
if (NOT CHECK_LINKER_FLAG)
  function(check_linker_flag)
  endfunction()
endif ()

# FORTIFY_SOURCE prevents overriding memcpy, disable memcpy if detected.
# This checks if memcpy can be overridden.
CHECK_CXX_SOURCE_COMPILES("
#include <string.h>
extern \"C\" void* memcpy(void *, const void*, size_t) {}
int main() { return 0; }
" SNMALLOC_MEMCPY_OVERRIDE)
if (NOT SNMALLOC_MEMCPY_OVERRIDE)
  message(WARNING "Unable to override memcpy (possbily due to _FORTIFY_SOURCE).")
  if (SNMALLOC_CHECK_LOADS)
    message(FATAL_ERROR "SNMALLOC_CHECK_LOADS is incompatible as memcpy could not be overridden. (possbily due to _FORTIFY_SOURCE)")
  endif()
endif()

# Detect if nostdlib++ works
# Use custom code to detect failure with glibcxx assertions define.
set(CMAKE_REQUIRED_LINK_OPTIONS -nostdlib++)
check_cxx_source_compiles("
#include <array>

char foo(std::array<int, 10> p, int i)
{ return p[i]; }

int main()
{ return 0; }"
 SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX)
set(CMAKE_REQUIRED_LINK_OPTIONS "")

# Detect if pthread_atfork works
CHECK_CXX_SOURCE_COMPILES("
#include <pthread.h>
void prepare() {}
void parent() {}
void child() {}
int main() {
  pthread_atfork(prepare, parent, child);
  return 0;
}
" SNMALLOC_PTHREAD_ATFORK_WORKS)


if (NOT MSVC AND NOT (SNMALLOC_CLEANUP STREQUAL CXX11_DESTRUCTORS))
  # If the target compiler doesn't support -nostdlib++ then we must enable C at
  # the global scope for the fallbacks to work.
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
#  include <sys/random.h>
#endif
int main() {
  int entropy = 0;
  int res = getentropy(&entropy, sizeof(entropy));
  return res;
}
" SNMALLOC_PLATFORM_HAS_GETENTROPY)

# check if linux/random.h is available
# older libcs might not have sys/random.h
# but some might provide the necessary flags via linux/random.h
# the __has_include macro isn't working properly on all platforms for that header
# this is why we check its existence here
CHECK_INCLUDE_FILE_CXX(linux/random.h SNMALLOC_HAS_LINUX_RANDOM_H)

# check if futex.h is available
CHECK_INCLUDE_FILE_CXX(linux/futex.h SNMALLOC_HAS_LINUX_FUTEX_H)

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
  # know whether our clang-format file will work with newer versions of the
  # tool.  It does not work with older versions as AfterCaseLabel is not supported
  # in earlier versions.
  find_program(CLANG_FORMAT NAMES
    clang-format150 clang-format-15)

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
  target_compile_features(snmalloc INTERFACE cxx_std_17)
else()
  target_compile_features(snmalloc INTERFACE cxx_std_20)
endif()

if(SNMALLOC_ENABLE_WAIT_ON_ADDRESS)
  target_compile_definitions(snmalloc INTERFACE SNMALLOC_USE_WAIT_ON_ADDRESS=1)
else()
  target_compile_definitions(snmalloc INTERFACE SNMALLOC_USE_WAIT_ON_ADDRESS=0)
endif()

if(SNMALLOC_REMOTE_BATCH_PROCESS_SIZE)
  target_compile_definitions(snmalloc INTERFACE SNMALLOC_REMOTE_BATCH_PROCESS_SIZE=${SNMALLOC_REMOTE_BATCH_PROCESS_SIZE})
endif()

if(SNMALLOC_USE_SELF_VENDORED_STL)
  target_compile_definitions(snmalloc INTERFACE SNMALLOC_USE_SELF_VENDORED_STL)
endif()

if (SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  if (NOT EXISTS ${SNMALLOC_GWP_ASAN_INCLUDE_PATH})
    message(FATAL_ERROR "GwpAsan cannot be enabled without setting SNMALLOC_GWP_ASAN_INCLUDE_PATH")
  endif()
  if (NOT EXISTS ${SNMALLOC_GWP_ASAN_LIBRARY_PATH})
    message(FATAL_ERROR "GwpAsan cannot be enabled without setting SNMALLOC_GWP_ASAN_LIBRARY_PATH")
  endif()
  message(STATUS "GwpAsan is enabled: ${SNMALLOC_GWP_ASAN_LIBRARY_PATH}/libclang_rt.gwp_asan-${CMAKE_SYSTEM_PROCESSOR}.a")
  target_include_directories(snmalloc INTERFACE ${SNMALLOC_GWP_ASAN_INCLUDE_PATH})
  target_link_directories(snmalloc INTERFACE ${SNMALLOC_GWP_ASAN_LIBRARY_PATH})
  target_compile_definitions(snmalloc INTERFACE -DSNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  target_link_libraries(snmalloc INTERFACE clang_rt.gwp_asan-${CMAKE_SYSTEM_PROCESSOR})
endif()

# https://learn.microsoft.com/en-us/cpp/build/reference/zc-cplusplus
if(MSVC)
  target_compile_options(snmalloc INTERFACE "/Zc:__cplusplus")
endif()

if (CMAKE_SYSTEM_NAME STREQUAL NetBSD)
	target_include_directories(snmalloc INTERFACE /usr/pkg/include)
	target_link_directories(snmalloc INTERFACE /usr/pkg/lib)
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
  target_compile_definitions(snmalloc INTERFACE $<$<BOOL:${WIN8COMPAT}>:WINVER=0x0603>)
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

if (NOT SNMALLOC_HEADER_ONLY_LIBRARY AND SNMALLOC_IPO)
  check_ipo_supported(RESULT HAS_IPO)
  if (HAS_IPO)
    set(SNMALLOC_COMPILER_SUPPORT_IPO TRUE)
  endif()
endif()

# Helper function that conditionally defines a macro for the build target if
# the CMake variable of the same name is set.
function(add_as_define FLAG)
  target_compile_definitions(snmalloc INTERFACE $<$<BOOL:${${FLAG}}>:${FLAG}>)
endfunction()
function(add_as_define_value KEY)
  if (NOT ${${KEY}} STREQUAL "")
    target_compile_definitions(snmalloc INTERFACE ${KEY}=${${KEY}})
  endif ()
endfunction()

add_as_define(SNMALLOC_QEMU_WORKAROUND)
add_as_define(SNMALLOC_TRACING)
add_as_define(SNMALLOC_CI_BUILD)
add_as_define(SNMALLOC_PLATFORM_HAS_GETENTROPY)
add_as_define(SNMALLOC_PTHREAD_ATFORK_WORKS)
add_as_define(SNMALLOC_HAS_LINUX_RANDOM_H)
add_as_define(SNMALLOC_HAS_LINUX_FUTEX_H)
if (SNMALLOC_NO_REALLOCARRAY)
  add_as_define(SNMALLOC_NO_REALLOCARRAY)
endif()
if (SNMALLOC_NO_REALLOCARR)
  add_as_define(SNMALLOC_NO_REALLOCARR)
endif()
add_as_define_value(SNMALLOC_MIN_ALLOC_SIZE)
add_as_define_value(SNMALLOC_MIN_ALLOC_STEP_SIZE)
add_as_define_value(SNMALLOC_DEALLOC_BATCH_RING_ASSOC)
add_as_define_value(SNMALLOC_DEALLOC_BATCH_RING_SET_BITS)

add_as_define_value(SNMALLOC_PAGESIZE)

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

if(NOT MSVC)
  check_linker_flag(CXX "-rdynamic" SNMALLOC_LINKER_SUPPORT_RDYNAMIC)
  if (SNMALLOC_LINKER_SUPPORT_RDYNAMIC)
    # Get better stack traces in CI and debug builds.
    target_link_options(snmalloc INTERFACE $<${ci_or_debug}:-rdynamic>)
  endif()
endif()

if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
  check_linker_flag(CXX "-Wl,--no-undefined" SNMALLOC_LINKER_SUPPORT_NO_ALLOW_SHLIB_UNDEF)
endif()

function(add_warning_flags name)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Zi /W4 /WX /wd4127 /wd4324 /wd4201>
    $<$<NOT:$<OR:$<CXX_COMPILER_ID:MSVC>,$<STREQUAL:${CMAKE_CXX_SIMULATE_ID},MSVC>>>:-fno-exceptions -fno-rtti -Wall -Wextra -Werror -Wundef>
    $<$<CXX_COMPILER_ID:Clang>:-Wsign-conversion -Wconversion>)
  target_link_options(${name} PRIVATE 
    $<$<BOOL:${SNMALLOC_LINKER_SUPPORT_NO_ALLOW_SHLIB_UNDEF}>:-Wl,--no-undefined>
    $<$<PLATFORM_ID:Windows>:$<${ci_or_debug}:/DEBUG>>)
endfunction()

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

  set(TESTDIR ${CMAKE_CURRENT_SOURCE_DIR}/src/test)

  if(SNMALLOC_BUILD_TESTING)
    enable_testing()
    subdirlist(TEST_CATEGORIES ${TESTDIR})
  else()
    set(TEST_CATEGORIES "")
  endif()
  list(REVERSE TEST_CATEGORIES)

  if (${SNMALLOC_CLEANUP} STREQUAL THREAD_CLEANUP)
    set(TEST_CLEANUP PTHREAD_DESTRUCTORS)
  else ()
    set(TEST_CLEANUP ${SNMALLOC_CLEANUP})
  endif()

  function(make_tests TAG DEFINES)
    foreach(TEST_CATEGORY ${TEST_CATEGORIES})
      message(VERBOSE "Adding ${TAG}/${TEST_CATEGORY} tests")
      subdirlist(TESTS ${TESTDIR}/${TEST_CATEGORY})
      foreach(TEST ${TESTS})
        unset(SRC)
        aux_source_directory(${TESTDIR}/${TEST_CATEGORY}/${TEST} SRC)
        set(TESTNAME "${TEST_CATEGORY}-${TEST}-${TAG}")

        add_executable(${TESTNAME} ${SRC})

        if(SNMALLOC_SANITIZER)
          target_compile_options(${TESTNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
          target_link_libraries(${TESTNAME} -fsanitize=${SNMALLOC_SANITIZER})
          if (${SNMALLOC_SANITIZER} MATCHES "thread")
            target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
          endif()
        endif()

        add_warning_flags(${TESTNAME})

        target_link_libraries(${TESTNAME} snmalloc)
        target_compile_definitions(${TESTNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
        
        if (NOT DEFINES STREQUAL " ")
          target_compile_definitions(${TESTNAME} PRIVATE ${DEFINES})
        endif()

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
      endforeach()
    endforeach()
  endfunction()

  if(NOT (DEFINED SNMALLOC_LINKER_FLAVOUR) OR ("${SNMALLOC_LINKER_FLAVOUR}" MATCHES "^$"))
    # Linker not specified externally; probe to see if we can make lld work
    set(CMAKE_REQUIRED_LINK_OPTIONS -fuse-ld=lld -Wl,--icf=all)
    check_cxx_source_compiles("int main() { return 1; }" LLD_WORKS)
    if (LLD_WORKS)
      message(STATUS "Using LLD to link snmalloc shims")
    endif()
  elseif(SNMALLOC_LINKER_FLAVOUR STREQUAL "lld")
    # Linker specified externally to be lld; assume it works and that the flags
    # have also been set for us
    set(LLD_WORKS TRUE)
  else()
    # Linker specified externally as something other than lld; presume it
    # doesn't work and don't add its flags, below
    set(LLD_WORKS FALSE)
  endif()

  if ((NOT CMAKE_SYSTEM_NAME STREQUAL "Haiku") AND (NOT SNMALLOC_ENABLE_DYNAMIC_LOADING))
    message(STATUS "snmalloc: Using static TLS model")
    set (SNMALLOC_STATIC_MODE_TLS TRUE)
  endif()

  if(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE)
    check_cxx_compiler_flag(-march=native SUPPORT_MARCH_NATIVE)
    if (NOT SUPPORT_MARCH_NATIVE)
      message(WARNING "Compiler does not support `-march=native` required by SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE")
      set(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE FALSE)
    endif()
endif()


  function(add_shim name type)
    add_library(${name} ${type} ${ARGN})
    target_link_libraries(${name} snmalloc)
    set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden INTERPROCEDURAL_OPTIMIZATION ${SNMALLOC_COMPILER_SUPPORT_IPO})
    target_compile_definitions(${name} PRIVATE "SNMALLOC_USE_${SNMALLOC_CLEANUP}")

    if(MSVC)
      target_compile_definitions(${name} PRIVATE -D_HAS_EXCEPTIONS=0)
    endif()

    add_warning_flags(${name})
    if(NOT MSVC)
      target_compile_definitions(${name} PRIVATE "SNMALLOC_EXPORT=__attribute__((visibility(\"default\")))")
      target_compile_options(${name} PRIVATE
        -fomit-frame-pointer -ffunction-sections)

      check_cxx_compiler_flag("-Werror -Wextra -Wall -mprfchw" SUPPORT_PREFETCH_WRITE)
      if (SUPPORT_PREFETCH_WRITE)
        target_compile_options(${name} PRIVATE -mprfchw)
      endif()
      # Static TLS model is unsupported on Haiku.
      if (SNMALLOC_STATIC_MODE_TLS)
        target_compile_options(${name} PRIVATE -ftls-model=initial-exec)
        target_compile_options(${name} PRIVATE $<$<BOOL:${SNMALLOC_CI_BUILD}>:-g>)
      endif()

      if(SNMALLOC_OPTIMISE_FOR_CURRENT_MACHINE)
        target_compile_options(${name} PRIVATE -march=native)
      endif()

      # Ensure that we do not link against C++ stdlib when compiling shims.
      # If the compiler supports excluding the C++ stdlib implementation, use
      # it.  Otherwise, fall back to linking the library as if it were C, which
      # has roughly the same effect.
      if (NOT ${SNMALLOC_CLEANUP} STREQUAL CXX11_DESTRUCTORS)
        if (SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX)
          target_link_options(${name} PRIVATE -nostdlib++)
        else()
          set_target_properties(${name} PROPERTIES LINKER_LANGUAGE C)
        endif()
      endif()
      # Remove all the duplicate new/malloc and free/delete definitions
      target_link_options(${name} PRIVATE $<$<BOOL:${LLD_WORKS}>:$<$<BOOL:${SNMALLOC_LINK_ICF}>:-Wl,--icf=all> -fuse-ld=lld>)
    endif()

    target_compile_definitions(${name} PRIVATE
      SNMALLOC_CHECK_LOADS=$<IF:$<BOOL:${SNMALLOC_CHECK_LOADS}>,true,false>)
    target_compile_definitions(${name} PRIVATE
      SNMALLOC_PAGEID=$<IF:$<BOOL:${SNMALLOC_PAGEID}>,true,false>)

    install(TARGETS ${name} EXPORT snmallocConfig)

  endfunction()

  set(SHIM_FILES src/snmalloc/override/malloc.cc src/snmalloc/override/new.cc)
  set(SHIM_FILES_MEMCPY src/snmalloc/override/memcpy.cc)

  add_shim(snmalloc-new-override STATIC src/snmalloc/override/new.cc)

  if (SNMALLOC_STATIC_LIBRARY)
    add_shim(snmallocshim-static STATIC ${SHIM_FILES})
    target_compile_definitions(snmallocshim-static PRIVATE
            SNMALLOC_STATIC_LIBRARY_PREFIX=${SNMALLOC_STATIC_LIBRARY_PREFIX})
  endif ()

  if(NOT WIN32)
    add_shim(snmallocshim SHARED ${SHIM_FILES})
    if (SNMALLOC_MEMCPY_OVERRIDE)
      add_shim(snmallocshim-checks-memcpy-only SHARED ${SHIM_FILES} ${SHIM_FILES_MEMCPY})
      add_shim(snmallocshim-checks SHARED ${SHIM_FILES} ${SHIM_FILES_MEMCPY})
    else()
    add_shim(snmallocshim-checks SHARED ${SHIM_FILES})
    endif()
    target_compile_definitions(snmallocshim-checks PRIVATE SNMALLOC_CHECK_CLIENT)
  endif()

  if(SNMALLOC_RUST_SUPPORT)
    add_shim(snmallocshim-rust STATIC src/snmalloc/override/rust.cc)
    add_shim(snmallocshim-checks-rust STATIC src/snmalloc/override/rust.cc)
    target_compile_definitions(snmallocshim-checks-rust PRIVATE SNMALLOC_CHECK_CLIENT)
    if (SNMALLOC_RUST_LIBC_API)
      target_compile_definitions(snmallocshim-rust PRIVATE SNMALLOC_RUST_LIBC_API)
      target_compile_definitions(snmallocshim-checks-rust PRIVATE SNMALLOC_RUST_LIBC_API)
    endif()
  endif()

  if (SNMALLOC_BUILD_TESTING)
    set(FLAVOURS fast;check)

    foreach(FLAVOUR ${FLAVOURS})
      if (${FLAVOUR} STREQUAL "check")
        set(DEFINES SNMALLOC_CHECK_CLIENT)
      endif()
      if (${FLAVOUR} STREQUAL "fast")
        set(DEFINES " ")
      endif()

      make_tests(${FLAVOUR} ${DEFINES})
    endforeach()
  endif()

  if (SNMALLOC_BENCHMARK_INDIVIDUAL_MITIGATIONS)
    set (MITIGATIONS 
      metadata_protection;
      pal_enforce_access;
      random_pagemap;
      sanity_checks;
      freelist_forward_edge;
      freelist_backward_edge;
      freelist_teardown_validate;
      reuse_LIFO;
      random_larger_thresholds;
      random_initial;
      random_preserve;
      random_extra_slab)


    foreach (MITIGATION ${MITIGATIONS})
      set(DEFINES "SNMALLOC_CHECK_CLIENT_MITIGATIONS=${MITIGATION}")
      add_shim(snmallocshim-${MITIGATION} SHARED ${SHIM_FILES})
      target_compile_definitions(snmallocshim-${MITIGATION} PRIVATE ${DEFINES})
      if (SNMALLOC_BUILD_TESTING)
        make_tests(${MITIGATION} ${DEFINES})
      endif()
    endforeach()

    set(MITIGATIONSET "no_checks")
    set(COUNT 0)
    foreach (MITIGATION ${MITIGATIONS})
      MATH(EXPR COUNT "${COUNT} + 1")
      set(MITIGATIONNAME "mitigations-${COUNT}")
      set(MITIGATIONSET "${MITIGATIONSET}+${MITIGATION}")
      message(STATUS "MITIGATIONSET: ${COUNT} -> ${MITIGATIONSET}")
      set(DEFINES "-DSNMALLOC_CHECK_CLIENT_MITIGATIONS=${MITIGATIONSET}")
      add_shim(snmallocshim-${MITIGATIONNAME} SHARED ${SHIM_FILES})
      target_compile_definitions(snmallocshim-${MITIGATIONNAME} PRIVATE ${DEFINES})
      if (SNMALLOC_BUILD_TESTING)
        make_tests(${MITIGATIONNAME} ${DEFINES})
      endif()
    endforeach()
  endif()

  if (SNMALLOC_BUILD_TESTING)
    clangformat_targets()
  endif ()
endif()

install(TARGETS snmalloc EXPORT snmallocConfig)

install(TARGETS EXPORT snmallocConfig DESTINATION ${CMAKE_INSTALL_LIBDIR}
  PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/snmalloc)

install(DIRECTORY src/snmalloc/aal DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/backend DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/backend_helpers DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/ds DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/ds_aal DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/ds_core DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/global DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/mem DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/override DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/pal DESTINATION include/snmalloc)
install(DIRECTORY src/snmalloc/stl DESTINATION include/snmalloc)
install(FILES
    src/test/measuretime.h
    src/test/opt.h
    src/test/setup.h
    src/test/usage.h
    src/test/xoroshiro.h
  DESTINATION include/snmalloc/test
)
install(FILES src/snmalloc/snmalloc.h;src/snmalloc/snmalloc_core.h;src/snmalloc/snmalloc_front.h DESTINATION include/snmalloc)

install(EXPORT snmallocConfig
  FILE snmalloc-config.cmake
  NAMESPACE snmalloc::
  DESTINATION "share/snmalloc"
)

if (SNMALLOC_ENABLE_FUZZING)
  add_subdirectory(fuzzing)
endif()
