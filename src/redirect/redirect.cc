
#include "snmalloc.h"

#define NAME(a) malloc_size_##a
#define STRINGIFY(a) a
#define NAME_STRING(a) NAME(a)

#ifdef WIN32
#  define REDIRECT_MALLOC_SIZE(a, b) \
    extern "C" void* NAME(a)(); \
    __pragma(comment(linker, "/alternatename:malloc_size_##a=malloc_size_##b"))
#else
#  define REDIRECT_MALLOC_SIZE(a, b) \
    __attribute__((alias(#b))) extern "C" void* a()
#endif

#define DEFINE_MALLOC_SIZE(name, s) \
  extern "C" void* name() \
  { \
    return snmalloc::ThreadAlloc::get_noncachable()->template alloc<s>(); \
  }

extern "C" void free_local_small(void* ptr)
{
  if (snmalloc::Alloc::small_local_dealloc(ptr))
    return;
  snmalloc::ThreadAlloc::get_noncachable()->small_local_dealloc_slow(ptr);
}

#  define GENERATE_FREE_SIZE(a) \
    __attribute__((alias("free_local_small"))) extern "C" void* a()

void* __stack_alloc_large(size_t size, size_t align)
{
  size_t asize = snmalloc::aligned_size(1ULL << align, size);
  return snmalloc::ThreadAlloc::get_noncachable()->alloc(asize);
}

void __stack_free_large(void* ptr, size_t size, size_t align)
{
  size_t asize = snmalloc::aligned_size(1ULL << align, size);
  snmalloc::ThreadAlloc::get_noncachable()->dealloc(ptr, asize);
}


#include "generated.cc"
