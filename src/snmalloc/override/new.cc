#include "snmalloc/snmalloc.h"

#include <new>

#ifdef _WIN32
#  ifdef __clang__
#    define EXCEPTSPEC noexcept
#  else
#    define EXCEPTSPEC
#  endif
#else
#  ifdef _GLIBCXX_USE_NOEXCEPT
#    define EXCEPTSPEC _GLIBCXX_USE_NOEXCEPT
#  elif defined(_NOEXCEPT)
#    define EXCEPTSPEC _NOEXCEPT
#  else
#    define EXCEPTSPEC
#  endif
#endif

// only define these if we are not using the vendored STL
#ifndef SNMALLOC_USE_SELF_VENDORED_STL
void* operator new(size_t size)
{
  return snmalloc::libc::malloc(size);
}

void* operator new[](size_t size)
{
  return snmalloc::libc::malloc(size);
}

void* operator new(size_t size, std::nothrow_t&)
{
  return snmalloc::libc::malloc(size);
}

void* operator new[](size_t size, std::nothrow_t&)
{
  return snmalloc::libc::malloc(size);
}

void operator delete(void* p) EXCEPTSPEC
{
  snmalloc::libc::free(p);
}

void operator delete(void* p, size_t size) EXCEPTSPEC
{
  snmalloc::libc::free_sized(p, size);
}

void operator delete(void* p, std::nothrow_t&)
{
  snmalloc::libc::free(p);
}

void operator delete[](void* p) EXCEPTSPEC
{
  snmalloc::libc::free(p);
}

void operator delete[](void* p, size_t size) EXCEPTSPEC
{
  snmalloc::libc::free_sized(p, size);
}

void operator delete[](void* p, std::nothrow_t&)
{
  snmalloc::libc::free(p);
}

void* operator new(size_t size, std::align_val_t val)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::libc::malloc(size);
}

void* operator new[](size_t size, std::align_val_t val)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::libc::malloc(size);
}

void* operator new(size_t size, std::align_val_t val, std::nothrow_t&)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::libc::malloc(size);
}

void* operator new[](size_t size, std::align_val_t val, std::nothrow_t&)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::libc::malloc(size);
}

void operator delete(void* p, std::align_val_t) EXCEPTSPEC
{
  snmalloc::libc::free(p);
}

void operator delete[](void* p, std::align_val_t) EXCEPTSPEC
{
  snmalloc::libc::free(p);
}

void operator delete(void* p, size_t size, std::align_val_t val) EXCEPTSPEC
{
  size = snmalloc::aligned_size(size_t(val), size);
  snmalloc::libc::free_sized(p, size);
}

void operator delete[](void* p, size_t size, std::align_val_t val) EXCEPTSPEC
{
  size = snmalloc::aligned_size(size_t(val), size);
  snmalloc::libc::free_sized(p, size);
}
#endif
