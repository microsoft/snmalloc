#include <thread>

#ifndef __has_feature
#  define __has_feature(x) 0
#endif

// These test partially override the libc malloc/free functions to test
// interesting corner cases.  This breaks the sanitizers as they will be
// partially overridden.  So we disable the tests if any of the sanitizers are
// enabled.
#if defined(__linux__) && !__has_feature(address_sanitizer) && \
  !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
  !defined(SNMALLOC_THREAD_SANITIZER_ENABLED)
#  define RUN_TEST
#endif

#ifdef RUN_TEST
#  include <snmalloc/snmalloc.h>
#  include <stdlib.h>

template<size_t N, size_t M = 0>
void thread_destruct()
{
  snmalloc::message("thread_destruct<{}, {}> start", N, M);
  static thread_local snmalloc::OnDestruct destruct{
    []() { snmalloc::message("thread_destruct<{}, {}> destructor", N, M); }};
  snmalloc::message("thread_destruct<{}, {}> end", N, M);

  if constexpr (N > M + 1)
  {
    // destructor
    thread_destruct<N, (M + N) / 2>();
    thread_destruct<(M + N) / 2, M>();
  }
}

// We only selectively override these functions. Otherwise, malloc may be called
// before atexit triggers the first initialization attempt.

extern "C" void* calloc(size_t num, size_t size)
{
  snmalloc::message("calloc({}, {})", num, size);
  return snmalloc::libc::calloc(num, size);
}

extern "C" void free(void* p)
{
  snmalloc::message("free({})", p);
  if (snmalloc::is_owned(p))
    return snmalloc::libc::free(p);
  // otherwise, just leak the memory
}

int main()
{
  std::thread(thread_destruct<1000>).join();
}
#else
int main()
{
  return 0;
}
#endif
