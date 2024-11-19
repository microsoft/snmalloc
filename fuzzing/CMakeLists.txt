include(FetchContent)

FetchContent_Declare(
  fuzztest
  GIT_REPOSITORY https://github.com/google/fuzztest.git
  GIT_TAG        2024-10-28
)

FetchContent_MakeAvailable(fuzztest)

enable_testing()
fuzztest_setup_fuzzing_flags()

add_executable(
  snmalloc-fuzzer
  snmalloc-fuzzer.cpp
)

target_link_libraries(snmalloc-fuzzer PRIVATE snmalloc)
target_compile_options(snmalloc-fuzzer PRIVATE -fsanitize=address -DADDRESS_SANITIZER)

link_fuzztest(snmalloc-fuzzer)
