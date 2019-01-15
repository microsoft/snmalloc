#include "../mem/alloc.h"
#include "../mem/threadalloc.h"
#include "../snmalloc.h"

#ifdef _WIN32
#  define EXCEPTSPEC
#else
#  ifdef _GLIBCXX_USE_NOEXCEPT
#    define EXCEPTSPEC _GLIBCXX_USE_NOEXCEPT
#  elif defined(_NOEXCEPT)
#    define EXCEPTSPEC _NOEXCEPT
#  else
#    define EXCEPTSPEC
#  endif
#endif

using namespace snmalloc;

void* operator new(size_t size)
{
  return ThreadAlloc::get()->alloc(size);
}

void* operator new[](size_t size)
{
  return ThreadAlloc::get()->alloc(size);
}

void* operator new(size_t size, std::nothrow_t&)
{
  return ThreadAlloc::get()->alloc(size);
}

void* operator new[](size_t size, std::nothrow_t&)
{
  return ThreadAlloc::get()->alloc(size);
}

void operator delete(void* p)EXCEPTSPEC
{
  ThreadAlloc::get()->dealloc(p);
}

void operator delete(void* p, size_t size)EXCEPTSPEC
{
  ThreadAlloc::get()->dealloc(p, size);
}

void operator delete(void* p, std::nothrow_t&)
{
  ThreadAlloc::get()->dealloc(p);
}

void operator delete[](void* p) EXCEPTSPEC
{
  ThreadAlloc::get()->dealloc(p);
}

void operator delete[](void* p, size_t size) EXCEPTSPEC
{
  ThreadAlloc::get()->dealloc(p, size);
}

void operator delete[](void* p, std::nothrow_t&)
{
  ThreadAlloc::get()->dealloc(p);
}
