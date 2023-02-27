#include "malloc.cc"

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

using namespace snmalloc;

void* operator new(size_t size)
{
  return ThreadAlloc::get().alloc(size);
}

void* operator new[](size_t size)
{
  return ThreadAlloc::get().alloc(size);
}

void* operator new(size_t size, std::nothrow_t&)
{
  return ThreadAlloc::get().alloc(size);
}

void* operator new[](size_t size, std::nothrow_t&)
{
  return ThreadAlloc::get().alloc(size);
}

void operator delete(void* p)EXCEPTSPEC
{
  ThreadAlloc::get().dealloc(p);
}

void operator delete(void* p, size_t size)EXCEPTSPEC
{
  if (p == nullptr)
    return;
  ThreadAlloc::get().dealloc(p, size);
}

void operator delete(void* p, std::nothrow_t&)
{
  ThreadAlloc::get().dealloc(p);
}

void operator delete[](void* p) EXCEPTSPEC
{
  ThreadAlloc::get().dealloc(p);
}

void operator delete[](void* p, size_t size) EXCEPTSPEC
{
  if (p == nullptr)
    return;
  ThreadAlloc::get().dealloc(p, size);
}

void operator delete[](void* p, std::nothrow_t&)
{
  ThreadAlloc::get().dealloc(p);
}

void* operator new(size_t size, std::align_val_t val)
{
  size = aligned_size(size_t(val), size);
  return ThreadAlloc::get().alloc(size);
}

void* operator new[](size_t size, std::align_val_t val)
{
  size = aligned_size(size_t(val), size);
  return ThreadAlloc::get().alloc(size);
}

void* operator new(size_t size, std::align_val_t val, std::nothrow_t&)
{
  size = aligned_size(size_t(val), size);
  return ThreadAlloc::get().alloc(size);
}

void* operator new[](size_t size, std::align_val_t val, std::nothrow_t&)
{
  size = aligned_size(size_t(val), size);
  return ThreadAlloc::get().alloc(size);
}

void operator delete(void* p, std::align_val_t)EXCEPTSPEC
{
  ThreadAlloc::get().dealloc(p);
}

void operator delete[](void* p, std::align_val_t) EXCEPTSPEC
{
  ThreadAlloc::get().dealloc(p);
}

void operator delete(void* p, size_t size, std::align_val_t val)EXCEPTSPEC
{
  size = aligned_size(size_t(val), size);
  ThreadAlloc::get().dealloc(p, size);
}

void operator delete[](void* p, size_t size, std::align_val_t val) EXCEPTSPEC
{
  if (p == nullptr)
    return;
  size = aligned_size(size_t(val), size);
  ThreadAlloc::get().dealloc(p, size);
}
