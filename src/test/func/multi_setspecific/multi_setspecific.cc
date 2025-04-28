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
#  include <array>
#  include <snmalloc/snmalloc.h>
#  include <stdlib.h>
#  include <thread>

// A key in the second "second level" block of the pthread key table.
// First second level block is statically allocated.
// This is be 33.
pthread_key_t key;

void thread_setspecific()
{
  // If the following line is uncommented then the test will pass.
  // free(calloc(1, 1));
  pthread_setspecific(key, (void*)1);
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
  // Just leak it
  if (snmalloc::is_owned(p))
    return snmalloc::libc::free(p);
}

void callback(void*)
{
  snmalloc::message("callback");
}

int main()
{
  // The first 32 keys are statically allocated, so we need to create 33 keys
  // to create a key for which pthread_setspecific will call the calloc.
  for (size_t i = 0; i < 33; i++)
  {
    pthread_key_create(&key, callback);
  }

  // The first calloc occurs here, after the first [0, 32] keys have been
  // created thus snmalloc will choose the key 33, `key` contains the key `32`
  // and snmalloc `33`. Both of these keys are not in the statically allocated
  // part of the pthread key space.
  std::thread(thread_setspecific).join();

  // There should be a single allocator that can be extracted.
  if (snmalloc::AllocPool<snmalloc::Config>::extract() == nullptr)
  {
    // The thread has not torn down its allocator.
    snmalloc::report_fatal_error(
      "Teardown of thread allocator has not occurred.");
    return 1;
  }

  return 0;
}
#else
int main()
{
  // This test is not run, but it is used to check that the code compiles.
  return 0;
}
#endif
