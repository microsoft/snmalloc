cmake_minimum_required(VERSION 3.21)
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
option(SNMALLOC_PTHREAD_FORK_PROTECTION "Guard against forking while allocator locks are held using pthread_atfork hooks" OFF)
option(SNMALLOC_ENABLE_FUZZING "Enable fuzzing instrumentation tests" OFF)
option(SNMALLOC_USE_SELF_VENDORED_STL "Avoid using system STL" OFF)
option(SNMALLOC_COVERAGE "Build with clang source-based coverage instrumentation" OFF)
option(SNMALLOC_PROFILE "Build with heap profiling support" OFF)
# Phase 9.2 (ticket 86aj0tr1e) -- per-thread frontend cache stats.
# When ON, `Allocator` carries a small `FrontendStats` struct with
# per-thread non-atomic counters for the fast/slow alloc/dealloc and
# cross-thread message paths.  Off by default so release builds compile
# to identical code (no new symbols, no new struct fields, no
# increment sites).  Aggregation lives in `stats_export.cc`.
option(SNMALLOC_STATS "Build with per-thread frontend cache statistics" OFF)
# Profile-guided optimization plumbing. The option itself is consumed by
# cmake/snmalloc_pgo.cmake (included further down, once the snmalloc
# target has been declared) so all targets in the build inherit the
# correct -fprofile-{generate,use} flags. See cmake/snmalloc_pgo.cmake
# and scripts/run-pgo-build.sh for the full two-stage workflow.
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

# Coverage instrumentation. Clang source-based coverage only; gcov
# format is not consumed by the diff pipeline.
if (SNMALLOC_COVERAGE)
  if (NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
           CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
      "SNMALLOC_COVERAGE requires Clang or AppleClang (got ${CMAKE_CXX_COMPILER_ID}). "
      "Point CMAKE_CXX_COMPILER at clang++ for the coverage build directory.")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

# Profile-guided optimization. Must come before any add_library/add_executable
# so the generate-stage instrumentation and use-stage layout decisions are
# applied to every object in the build.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/snmalloc_pgo.cmake)

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

if (SNMALLOC_PTHREAD_FORK_PROTECTION AND NOT SNMALLOC_PTHREAD_ATFORK_WORKS)
  message(FATAL_ERROR "SNMALLOC_PTHREAD_FORK_PROTECTION requires working pthread_atfork support")
endif()

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

# clangformat_targets([VERSION <ver>]
#                     [TARGET_NAME <name>]
#                     [INCLUDE_PATTERNS <glob> ...]
#                     [EXCLUDE_PATTERNS <regex> ...])
#
# Generate a CMake target that runs clang-format on a set of source files.
# A companion <name>-check target is also created that verifies formatting
# without modifying files (--dry-run --Werror), suitable for CI.
#
# Options:
#   VERSION          - clang-format version to search for (default: 15).
#                      Falls back to the unversioned 'clang-format' with a
#                      warning if the requested version is not found.
#   TARGET_NAME      - Name of the generated targets (default: clangformat).
#   INCLUDE_PATTERNS - Glob patterns passed to file(GLOB_RECURSE) to collect
#                      source files (default: src/*.cc src/*.h src/*.hh).
#   EXCLUDE_PATTERNS - Regex patterns used to exclude files from the collected
#                      list (default: src/[^/]*/[^/]*_concept\.h$).
function(clangformat_targets)
  cmake_parse_arguments(
    CLANGFMT
    ""
    "VERSION;TARGET_NAME"
    "INCLUDE_PATTERNS;EXCLUDE_PATTERNS"
    ${ARGN}
  )

  # Default inclusion globs match this project's source layout.
  if (NOT CLANGFMT_INCLUDE_PATTERNS)
    set(CLANGFMT_INCLUDE_PATTERNS src/*.cc src/*.h src/*.hh)
  endif()

  # Default exclusion regexes skip concept headers that clang-format
  # does not yet handle well.  See https://reviews.llvm.org/D79773
  if (NOT CLANGFMT_EXCLUDE_PATTERNS)
    set(CLANGFMT_EXCLUDE_PATTERNS "src/[^/]*/[^/]*_concept\\.h$")
  endif()

  # Default clang-format version.
  if (NOT CLANGFMT_VERSION)
    set(CLANGFMT_VERSION 15)
  endif()

  # Default target name.
  if (NOT CLANGFMT_TARGET_NAME)
    set(CLANGFMT_TARGET_NAME clangformat)
  endif()

  # The clang-format tool is installed under a variety of different names.  Try
  # to find a sensible one.  Look for the requested version first, then fall
  # back to the unversioned name.
  find_program(CLANG_FORMAT NAMES
    clang-format${CLANGFMT_VERSION}0
    clang-format-${CLANGFMT_VERSION})

  if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
    find_program(CLANG_FORMAT NAMES clang-format)
    if (NOT ${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
      message(WARNING "Could not find clang-format version ${CLANGFMT_VERSION}, falling back to ${CLANG_FORMAT}")
    endif()
  endif()

  # If we've found a clang-format tool, generate a target for it, otherwise emit
  # a warning.
  if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
    message(WARNING "Not generating ${CLANGFMT_TARGET_NAME} target, no clang-format tool found")
  else ()
    message(STATUS "Generating ${CLANGFMT_TARGET_NAME} target using ${CLANG_FORMAT}")
    file(GLOB_RECURSE ALL_SOURCE_FILES CONFIGURE_DEPENDS ${CLANGFMT_INCLUDE_PATTERNS})
    foreach(pattern IN LISTS CLANGFMT_EXCLUDE_PATTERNS)
      list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX "${pattern}")
    endforeach()
    add_custom_target(
      ${CLANGFMT_TARGET_NAME}
      COMMAND ${CLANG_FORMAT}
      -i
      ${ALL_SOURCE_FILES})
    add_custom_target(
      ${CLANGFMT_TARGET_NAME}-check
      COMMAND ${CLANG_FORMAT}
      --dry-run --Werror
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
    $<INSTALL_INTERFACE:include>
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

option(SNMALLOC_ENABLE_PAC
  "Use Pointer Authentication (PAC) for freelist forward-edge protection \
   Switch to defaul if target does not support it." OFF)

if(SNMALLOC_ENABLE_PAC)
  check_cxx_compiler_flag(
    "-Werror -fptrauth-intrinsics"
    SNMALLOC_COMPILER_SUPPORT_FPTRAUTH_INTRINSICS)
  if(SNMALLOC_COMPILER_SUPPORT_FPTRAUTH_INTRINSICS)
    set(SNMALLOC_SAVED_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fptrauth-intrinsics")
    check_cxx_source_compiles("
#if !defined(__aarch64__) && !defined(_M_ARM64) && !defined(_M_ARM64EC)
#  error PAC instructions require AArch64
#endif
#if !__has_include(<ptrauth.h>)
#  error Missing ptrauth.h
#endif
#include <ptrauth.h>
#ifndef __ARM_FEATURE_PAUTH
#  error Compiler target does not expose pointer authentication
#endif
static inline void* sign_and_auth(void* ptr, void* storage)
{
  auto discriminator = ptrauth_blend_discriminator(storage, 0x5678U);
  auto signed_ptr = ptrauth_sign_unauthenticated(
    ptr, ptrauth_key_process_dependent_data, discriminator);
  return ptrauth_auth_data(
    signed_ptr, ptrauth_key_process_dependent_data, discriminator);
}
int main()
{
  return sign_and_auth(nullptr, reinterpret_cast<void*>(0x1234UL)) != nullptr;
}
" SNMALLOC_COMPILER_SUPPORT_PACA_PACG)
    set(CMAKE_REQUIRED_FLAGS "${SNMALLOC_SAVED_REQUIRED_FLAGS}")
    unset(SNMALLOC_SAVED_REQUIRED_FLAGS)
  endif()

  if(NOT SNMALLOC_COMPILER_SUPPORT_PACA_PACG)
    message(FATAL_ERROR
      "SNMALLOC_ENABLE_PAC=ON but the target does not support.")
  endif()

  target_compile_options(snmalloc INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-fptrauth-intrinsics>)
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
add_as_define(SNMALLOC_PROFILE)
add_as_define(SNMALLOC_STATS)
add_as_define(SNMALLOC_CI_BUILD)
add_as_define(SNMALLOC_PTHREAD_FORK_PROTECTION)
add_as_define(SNMALLOC_PLATFORM_HAS_GETENTROPY)
add_as_define(SNMALLOC_PTHREAD_ATFORK_WORKS)
add_as_define(SNMALLOC_HAS_LINUX_RANDOM_H)
add_as_define(SNMALLOC_HAS_LINUX_FUTEX_H)
add_as_define(SNMALLOC_COMPILER_SUPPORT_PACA_PACG)
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
    # Export all symbols from executables so that statically linked operator
    # new/delete and malloc/free overrides are visible to shared libraries
    # (e.g. libstdc++) that call through PLT. Also gives better stack traces.
    target_link_options(snmalloc INTERFACE -rdynamic)
  endif()
endif()

if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
  check_linker_flag(CXX "-Wl,--no-undefined" SNMALLOC_LINKER_SUPPORT_NO_ALLOW_SHLIB_UNDEF)
endif()

function(add_warning_flags name)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/Zi /W4 /WX /wd4127 /wd4324 /wd4201>
    $<$<NOT:$<OR:$<CXX_COMPILER_ID:MSVC>,$<STREQUAL:${CMAKE_CXX_SIMULATE_ID},MSVC>>>:-fno-rtti -Wall -Wextra -Werror -Wundef>
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

  # Tests that only include snmalloc_testlib.h (no snmalloc.h or snmalloc_core.h).
  # These are mitigation-independent and can be compiled once, then linked
  # against both fast and check testlib variants.
  set(TESTLIB_ONLY_TESTS
    bits first_operation memory memory_usage multi_atexit multi_threadatexit
    profile_sampler
    redblack statistics teardown
    contention external_pointer large_alloc lotsofthreads post_teardown
    singlethread startup stack_walker_bench
  )

  function(make_tests TAG DEFINES)
    foreach(TEST_CATEGORY ${TEST_CATEGORIES})
      message(VERBOSE "Adding ${TAG}/${TEST_CATEGORY} tests")
      subdirlist(TESTS ${TESTDIR}/${TEST_CATEGORY})
      foreach(TEST ${TESTS})
        unset(SRC)
        aux_source_directory(${TESTDIR}/${TEST_CATEGORY}/${TEST} SRC)
        set(TESTNAME "${TEST_CATEGORY}-${TEST}-${TAG}")

        # Check if this test is testlib-only (mitigation-independent).
        # If so, compile the source once into an object library and reuse
        # across flavours.
        list(FIND TESTLIB_ONLY_TESTS ${TEST} _testlib_only_idx)
        set(OBJLIBNAME "${TEST_CATEGORY}-${TEST}-obj")
        if (NOT ${_testlib_only_idx} EQUAL -1 AND NOT TARGET ${OBJLIBNAME})
          # First flavour to see this test creates the object library.
          add_library(${OBJLIBNAME} OBJECT ${SRC})
          target_link_libraries(${OBJLIBNAME} snmalloc)
          target_compile_definitions(${OBJLIBNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
          add_warning_flags(${OBJLIBNAME})
          if(SNMALLOC_SANITIZER)
            target_compile_options(${OBJLIBNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
            if (${SNMALLOC_SANITIZER} MATCHES "thread")
              target_compile_definitions(${OBJLIBNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
            endif()
          endif()
        endif()

        if (NOT ${_testlib_only_idx} EQUAL -1)
          # Testlib-only: link the shared object library against this
          # flavour's testlib.
          add_executable(${TESTNAME} $<TARGET_OBJECTS:${OBJLIBNAME}>)
        else()
          add_executable(${TESTNAME} ${SRC})
        endif()

        # Track every test binary in a global property so the `coverage`
        # custom target can pass them all to llvm-cov without globbing.
        set_property(GLOBAL APPEND PROPERTY SNMALLOC_TEST_BINARIES ${TESTNAME})

        if(SNMALLOC_SANITIZER)
          target_compile_options(${TESTNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
          target_link_libraries(${TESTNAME} -fsanitize=${SNMALLOC_SANITIZER})
          if (${SNMALLOC_SANITIZER} MATCHES "thread")
            target_compile_definitions(${TESTNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
          endif()
        endif()

        add_warning_flags(${TESTNAME})

        target_link_libraries(${TESTNAME} snmalloc)
        # Link the pre-compiled test library into all tests.  For tests
        # that include snmalloc.h directly the linker will simply discard
        # the duplicate inline definitions (ODR-safe).
        target_link_libraries(${TESTNAME} snmalloc-testlib-${TAG})
        target_compile_definitions(${TESTNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
        
        if (NOT DEFINES STREQUAL " ")
          target_compile_definitions(${TESTNAME} PRIVATE ${DEFINES})
        endif()

        if (${TEST} MATCHES "release-.*")
          message(VERBOSE "Adding test: ${TESTNAME} only for release configs")
          add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} --smoke
                   CONFIGURATIONS "Release")
        else()
          message(VERBOSE "Adding test: ${TESTNAME}")
          add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} --smoke)
        endif()
        if (${TEST_CATEGORY} MATCHES "perf")
          message(VERBOSE "Single threaded test: ${TESTNAME}")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 4)
        endif()
        if (SNMALLOC_COVERAGE)
          # Per-test LLVM_PROFILE_FILE so cached coverage runs can
          # invalidate one test's profiles without touching the rest.
          set_tests_properties(${TESTNAME} PROPERTIES ENVIRONMENT
            "LLVM_PROFILE_FILE=${CMAKE_BINARY_DIR}/profiles/${TESTNAME}/%p-%m.profraw")
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

  function(build_test_library TAG DEFINES)
    set(LIBNAME snmalloc-testlib-${TAG})
    add_library(${LIBNAME} STATIC src/test/snmalloc_testlib.cc)
    target_link_libraries(${LIBNAME} PRIVATE snmalloc)
    set_target_properties(${LIBNAME} PROPERTIES INTERFACE_LINK_LIBRARIES "")
    target_compile_definitions(${LIBNAME} PRIVATE "SNMALLOC_USE_${TEST_CLEANUP}")
    if (NOT DEFINES STREQUAL " ")
      target_compile_definitions(${LIBNAME} PRIVATE ${DEFINES})
    endif()
    if(SNMALLOC_SANITIZER)
      target_compile_options(${LIBNAME} PRIVATE -g -fsanitize=${SNMALLOC_SANITIZER} -fno-omit-frame-pointer)
      if (${SNMALLOC_SANITIZER} MATCHES "thread")
        target_compile_definitions(${LIBNAME} PRIVATE SNMALLOC_THREAD_SANITIZER_ENABLED)
      endif()
    endif()
    add_warning_flags(${LIBNAME})
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


  function(compile name TYPE ${ARGN})
    add_library(${name} ${TYPE} ${ARGN})
    target_link_libraries(${name} PRIVATE snmalloc)

    # PRIVATE linking still touches INTERFACE level linking for unknown reasons.
    # Check https://github.com/microsoft/snmalloc/pull/799 for details.
    set_target_properties(${name} PROPERTIES INTERFACE_LINK_LIBRARIES "")

    target_compile_definitions(${name} PRIVATE "SNMALLOC_USE_${SNMALLOC_CLEANUP}")

    add_warning_flags(${name})
    if(NOT MSVC)
      target_compile_definitions(${name} PRIVATE "SNMALLOC_EXPORT=__attribute__((visibility(\"default\")))")
      set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden INTERPROCEDURAL_OPTIMIZATION ${SNMALLOC_COMPILER_SUPPORT_IPO})
      # Make debugging harder, and code faster.
      target_compile_options(${name} PRIVATE -fomit-frame-pointer)

      set_property(TARGET ${name} PROPERTY POSITION_INDEPENDENT_CODE ON)

      # Check for prefetch write support.
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

      # Remove all the duplicate new/malloc and free/delete definitions
      target_compile_options(${name} PRIVATE -ffunction-sections)
      target_link_options(${name} PRIVATE $<$<BOOL:${LLD_WORKS}>:$<$<BOOL:${SNMALLOC_LINK_ICF}>:-Wl,--icf=all> -fuse-ld=lld>)
    endif()

    target_compile_definitions(${name} PRIVATE
      SNMALLOC_CHECK_LOADS=$<IF:$<BOOL:${SNMALLOC_CHECK_LOADS}>,true,false>)
    target_compile_definitions(${name} PRIVATE
      SNMALLOC_PAGEID=$<IF:$<BOOL:${SNMALLOC_PAGEID}>,true,false>)

    if(MSVC)
    else()
      # We can't do --nostdlib++ as we are using exceptions, but we 
      # can make it a static dependency, which we aren't really using
      # as the interesting stuff for exceptions is in libgcc_s
      target_link_options(${name} PRIVATE -static-libstdc++)
    endif()

    if (SNMALLOC_RUST_LIBC_API)
      target_compile_definitions(${name} PRIVATE SNMALLOC_RUST_LIBC_API)
    endif()
    install(TARGETS ${name} EXPORT snmallocConfig)
  endfunction()

  # Various files for overriding libc/rust behaviours.
  set(MALLOC src/snmalloc/override/malloc.cc)
  set(NEW src/snmalloc/override/new.cc)
  set(MEMCPY src/snmalloc/override/memcpy.cc)
  # Phase 9.1: stats_export.cc carries the `snmalloc_get_full_stats` C
  # ABI symbol consumed by the Rust binding (and by any other C/C++
  # consumer of the libsnmalloc shims).  Wired into both the Rust
  # static library targets and the libc shim so the symbol ships
  # alongside the rest of the export surface on Linux/macOS.  Wave-2
  # Phase 9 tickets populate additional fields without changing the
  # file list.
  set(STATS_EXPORT src/snmalloc/override/stats_export.cc)
  # Phase 9.7: runtime_config.cc carries the C ABI shims
  # (`snmalloc_{set,get}_sample_interval` / `_decay_rate` /
  # `_max_local_cache`) backing `snmalloc::RuntimeConfig`.  Linked in
  # alongside stats_export.cc into both the Rust shim and the libc
  # shim so the tunables are available in every build flavour, with
  # or without `SNMALLOC_PROFILE` / `SNMALLOC_STATS`.
  set(RUNTIME_CONFIG src/snmalloc/override/runtime_config.cc)
  set(RUST src/snmalloc/override/rust.cc ${STATS_EXPORT} ${RUNTIME_CONFIG})

  set(ALLOC ${MALLOC} ${NEW} ${STATS_EXPORT} ${RUNTIME_CONFIG})
  set(ALL ${ALLOC} ${MEMCPY})

  if (SNMALLOC_STATIC_LIBRARY)
    compile(snmallocshim-static STATIC ${ALLOC})
    target_compile_definitions(snmallocshim-static PRIVATE
            SNMALLOC_STATIC_LIBRARY_PREFIX=${SNMALLOC_STATIC_LIBRARY_PREFIX})
  endif ()

  compile(snmalloc-new-override STATIC ${NEW})

  if(NOT WIN32)
    compile(snmallocshim SHARED ${ALLOC})

    if (SNMALLOC_MEMCPY_OVERRIDE)
      compile(snmallocshim-checks-memcpy-only SHARED ${ALL})
      compile(snmallocshim-checks SHARED ${ALL})
    else()
      compile(snmallocshim-checks SHARED ${ALLOC})
    endif()
    target_compile_definitions(snmallocshim-checks PRIVATE SNMALLOC_CHECK_CLIENT)
  
    compile(snmalloc-minimal SHARED ${MALLOC})
    target_compile_options(snmalloc-minimal PRIVATE -fno-exceptions)
    if (SNMALLOC_LINKER_SUPPORT_NOSTDLIBXX AND NOT ${SNMALLOC_CLEANUP} STREQUAL CXX11_DESTRUCTORS)
      target_compile_options(snmalloc-minimal PRIVATE -fno-exceptions -nostdlib++)
    endif ()
  endif()

  if(SNMALLOC_RUST_SUPPORT)
    compile(snmallocshim-rust STATIC ${RUST})
    compile(snmallocshim-checks-rust STATIC ${RUST})
    target_compile_definitions(snmallocshim-checks-rust PRIVATE SNMALLOC_CHECK_CLIENT)
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

      build_test_library(${FLAVOUR} ${DEFINES})
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
      compile(snmallocshim-${MITIGATION} SHARED ${ALLOC})
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
      compile(snmallocshim-${MITIGATIONNAME} SHARED ${ALLOC})
      target_compile_definitions(snmallocshim-${MITIGATIONNAME} PRIVATE ${DEFINES})
      if (SNMALLOC_BUILD_TESTING)
        make_tests(${MITIGATIONNAME} ${DEFINES})
      endif()
    endforeach()
  endif()

  if (SNMALLOC_BUILD_TESTING)
    clangformat_targets()
  endif ()

  # Coverage target. Generates a unified coverage.profdata +
  # coverage.json for all test binaries.
  if (SNMALLOC_COVERAGE AND SNMALLOC_BUILD_TESTING)
    find_program(LLVM_PROFDATA NAMES llvm-profdata-19 llvm-profdata-15 llvm-profdata)
    find_program(LLVM_COV      NAMES llvm-cov-19      llvm-cov-15      llvm-cov)
    if (LLVM_PROFDATA STREQUAL "LLVM_PROFDATA-NOTFOUND" OR
        LLVM_COV      STREQUAL "LLVM_COV-NOTFOUND")
      message(FATAL_ERROR
        "SNMALLOC_COVERAGE requires llvm-profdata and llvm-cov on PATH.")
    endif()

    get_property(_test_binaries GLOBAL PROPERTY SNMALLOC_TEST_BINARIES)
    if (NOT _test_binaries)
      message(FATAL_ERROR "SNMALLOC_TEST_BINARIES is empty; nothing to cover.")
    endif()

    # Build the -object argument list: first binary positional, rest with
    # -object prefix per llvm-cov's CLI. CMake list joined with spaces so
    # the resulting string can be embedded in `sh -c "..."` commands.
    set(_cov_objects "")
    set(_first TRUE)
    foreach(_b ${_test_binaries})
      if (_first)
        string(APPEND _cov_objects " $<TARGET_FILE:${_b}>")
        set(_first FALSE)
      else()
        string(APPEND _cov_objects " -object $<TARGET_FILE:${_b}>")
      endif()
    endforeach()

    # Build "<name>\t<path>" pairs for the per-test cache loop.
    set(_cov_test_pairs "")
    foreach(_b ${_test_binaries})
      string(APPEND _cov_test_pairs "${_b}\t$<TARGET_FILE:${_b}>\n")
    endforeach()

    set(_cov_profile_dir ${CMAKE_BINARY_DIR}/profiles)
    set(_cov_profdata    ${CMAKE_BINARY_DIR}/coverage.profdata)
    set(_cov_json        ${CMAKE_BINARY_DIR}/coverage.json)
    set(_cov_report      ${CMAKE_BINARY_DIR}/coverage.report.txt)
    set(_cov_pairs_file  ${CMAKE_BINARY_DIR}/coverage_tests.tsv)
    set(_cov_runner      ${CMAKE_SOURCE_DIR}/cmake/run_coverage.cmake)

    file(GENERATE OUTPUT ${_cov_pairs_file} CONTENT "${_cov_test_pairs}")

    add_custom_target(coverage
      COMMAND ${CMAKE_COMMAND}
        -DPROFROOT=${_cov_profile_dir}
        -DPAIRS=${_cov_pairs_file}
        -DCTEST=${CMAKE_CTEST_COMMAND}
        -DLLVM_PROFDATA=${LLVM_PROFDATA}
        -DLLVM_COV=${LLVM_COV}
        "-DCOV_OBJECTS=${_cov_objects}"
        -DCOV_PROFDATA=${_cov_profdata}
        -DCOV_JSON=${_cov_json}
        -DCOV_REPORT=${_cov_report}
        -DCTEST_JOBS=8
        -P ${_cov_runner}
      DEPENDS ${_test_binaries} ${_cov_runner}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      USES_TERMINAL
      COMMENT "Running ctest under LLVM coverage (cached) and exporting JSON")
  endif()
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
install(DIRECTORY src/snmalloc/mitigations DESTINATION include/snmalloc)
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

# Branch-hint inventory sidecar (Phase 10.2).
#
# Emits a JSON map of every SNMALLOC_LIKELY(...) / SNMALLOC_UNLIKELY(...)
# call site in src/snmalloc/. snmalloc-tools (Phase 10.4) consumes this to
# convert raw branch-miss IPs from `perf record -e branch-misses` into
# semantic "this hint was inverted" findings.
#
# Kept as a stand-alone target (not wired into the main library build) so
# that a missing Python interpreter never blocks ordinary builds. CMake's
# FindPython3 is tried optionally; if not found we skip the target with a
# status message rather than failing configuration.
set(SNMALLOC_BRANCH_HINTS_JSON "${CMAKE_BINARY_DIR}/snmalloc_branch_hints.json")
find_package(Python3 COMPONENTS Interpreter QUIET)
if (Python3_Interpreter_FOUND)
  add_custom_command(
    OUTPUT ${SNMALLOC_BRANCH_HINTS_JSON}
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/scripts/dump_branch_hints.py
      --repo-root ${CMAKE_SOURCE_DIR}
      --pretty
      -o ${SNMALLOC_BRANCH_HINTS_JSON}
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/dump_branch_hints.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Dumping SNMALLOC_LIKELY/UNLIKELY inventory to ${SNMALLOC_BRANCH_HINTS_JSON}"
    VERBATIM)
  add_custom_target(branch_hints_inventory
    DEPENDS ${SNMALLOC_BRANCH_HINTS_JSON})
  # Best-effort install. The sidecar is small and harmless when present, and
  # downstream tooling (snmalloc-tools, snmalloc-rs build.rs) looks for it
  # under share/snmalloc/.
  install(FILES ${SNMALLOC_BRANCH_HINTS_JSON}
    DESTINATION share/snmalloc
    OPTIONAL)
else()
  message(STATUS
    "Python3 not found; skipping branch_hints_inventory target. "
    "Build will succeed without the snmalloc_branch_hints.json sidecar.")
endif()

if (SNMALLOC_ENABLE_FUZZING)
  add_subdirectory(fuzzing)
endif()
