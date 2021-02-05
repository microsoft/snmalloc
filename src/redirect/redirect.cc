
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

#define DEFINE_MALLOC_SIZE(a, s) \
  extern "C" void* a() \
  { \
    return snmalloc::ThreadAlloc::get_noncachable()->template alloc<s>(); \
  }

#include "generated.cc"
