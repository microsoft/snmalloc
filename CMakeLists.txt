cmake_minimum_required(VERSION 3.8)
project(snmalloc C CXX)

option(USE_SNMALLOC_STATS "Track allocation stats" OFF)
option(USE_MEASURE "Measure performance with histograms" OFF)
option(EXPOSE_EXTERNAL_PAGEMAP "Expose the global pagemap" OFF)
option(EXPOSE_EXTERNAL_RESERVE "Expose an interface to reserve memory using the default memory provider" OFF)

set(CACHE_FRIENDLY_OFFSET OFF CACHE STRING "Base offset to place linked-list nodes.")

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
    add_compile_options(-Wall -Wextra -Werror)
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
  target_compile_options(snmalloc_lib INTERFACE -mcx16)
else()
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

# Have to set this globally, as can't be set on an interface target.
set(CMAKE_CXX_STANDARD 17)

if(USE_SNMALLOC_STATS)
  target_compile_definitions(snmalloc_lib INTERFACE -DUSE_SNMALLOC_STATS)
endif()

if(USE_MEASURE)
  target_compile_definitions(snmalloc_lib INTERFACE -DUSE_MEASURE)
endif()

# To build with just the header library target define SNMALLOC_ONLY_HEADER_LIBRARY
# in containing Cmake file.
if(NOT DEFINED SNMALLOC_ONLY_HEADER_LIBRARY)

  warnings_high()

  if(MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG")
  else()
    add_compile_options(-march=native -fno-exceptions -fno-rtti -g)
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

  macro(add_shim name)
    add_library(${name} SHARED src/override/malloc.cc)
    target_link_libraries(${name} snmalloc_lib)
    target_compile_definitions(${name} PRIVATE "SNMALLOC_EXPORT=__attribute__((visibility(\"default\")))")
    set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden)

    if(CACHE_FRIENDLY_OFFSET)
      target_compile_definitions(${name} PRIVATE -DCACHE_FRIENDLY_OFFSET=${CACHE_FRIENDLY_OFFSET})
    endif()

    if(EXPOSE_EXTERNAL_PAGEMAP)
      target_compile_definitions(${name} PRIVATE -DSNMALLOC_EXPOSE_PAGEMAP)
    endif()

    if(EXPOSE_EXTERNAL_RESERVE)
      target_compile_definitions(${name} PRIVATE -DSNMALLOC_EXPOSE_RESERVE)
    endif()
  endmacro()

  if(NOT MSVC)
    add_shim(snmallocshim)

    add_shim(snmallocshim-1mib)
    target_compile_definitions(snmallocshim-1mib PRIVATE IS_ADDRESS_SPACE_CONSTRAINED)
  endif()

  enable_testing()

  set(TESTDIR ${CMAKE_CURRENT_SOURCE_DIR}/src/test)
  subdirlist(TEST_CATEGORIES ${TESTDIR})
  list(REVERSE TEST_CATEGORIES)
  foreach(TEST_CATEGORY ${TEST_CATEGORIES})
    subdirlist(TESTS ${TESTDIR}/${TEST_CATEGORY})
    foreach(TEST ${TESTS})
      foreach(SUPER_SLAB_SIZE 1;16)
        unset(SRC)
        aux_source_directory(${TESTDIR}/${TEST_CATEGORY}/${TEST} SRC)
        set(TESTNAME "${TEST_CATEGORY}-${TEST}-${SUPER_SLAB_SIZE}")

        add_executable(${TESTNAME} ${SRC} src/override/new.cc)
        if (${SUPER_SLAB_SIZE} EQUAL 1)
          target_compile_definitions(${TESTNAME} PRIVATE IS_ADDRESS_SPACE_CONSTRAINED)
        endif()
        target_include_directories(${TESTNAME} PRIVATE src)
        target_link_libraries(${TESTNAME} snmalloc_lib)
        if (${TEST} MATCHES "release-.*")
          message(STATUS "Adding test: ${TESTNAME} only for release configs")
          add_test(NAME ${TESTNAME} COMMAND ${TESTNAME} CONFIGURATIONS "Release")
        else()
          message(STATUS "Adding test: ${TESTNAME}")
          add_test(${TESTNAME} ${TESTNAME})
        endif()
        if (${TEST_CATEGORY} MATCHES "perf")
          set_tests_properties(${TESTNAME} PROPERTIES PROCESSORS 3)
        endif()
      endforeach()
    endforeach()
  endforeach()

  # The clang-format tool is installed under a variety of different names.  Try
  # to find a sensible one.  Only look for 6.0 and 7.0 versions explicitly - we
  # don't know whether our clang-format file will work with newer versions of the
  # tool
  set(CLANG_FORMAT_NAMES
    clang-format-7.0
    clang-format-6.0
    clang-format70
    clang-format60
    clang-format)

  # Loop over each of the possible names of clang-format and try to find one.
  set(CLANG_FORMAT CLANG_FORMAT-NOTFOUND)
  foreach (NAME IN ITEMS ${CLANG_FORMAT_NAMES})
    if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
      find_program(CLANG_FORMAT ${NAME})
    endif ()
  endforeach()

  # If we've found a clang-format tool, generate a target for it, otherwise emit
  # a warning.
  if (${CLANG_FORMAT} STREQUAL "CLANG_FORMAT-NOTFOUND")
    message(WARNING "Not generating clangformat target, no clang-format tool found")
  else ()
    message(STATUS "Generating clangformat target using ${CLANG_FORMAT}")
    file(GLOB_RECURSE ALL_SOURCE_FILES *.cc *.h *.hh)
    add_custom_target(
      clangformat
      COMMAND ${CLANG_FORMAT}
      -i
      ${ALL_SOURCE_FILES})
  endif()
endif()
