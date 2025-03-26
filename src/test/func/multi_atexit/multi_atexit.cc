#ifndef __has_feature
#  define __has_feature(x) 0
#endif

#if defined(__linux__) && !__has_feature(address_sanitizer) && \
  !defined(__SANITIZE_ADDRESS__)
#  define RUN_TEST
#endif

#ifdef RUN_TEST
#  include <snmalloc/snmalloc.h>
#  include <stdlib.h>

void do_nothing() {}

// We only selectively override these functions. Otherwise, malloc may be called
// before atexit triggers the first initialization attempt.

extern "C" void* calloc(size_t num, size_t size)
{
  return snmalloc::libc::calloc(num, size);
}

extern "C" void free(void* p)
{
  if (snmalloc::is_owned(p))
    return snmalloc::libc::free(p);
  // otherwise, just leak the memory
}

#endif

int main()
{
#ifdef RUN_TEST
  for (int i = 0; i < 8192; ++i)
    atexit(do_nothing);
#endif
}