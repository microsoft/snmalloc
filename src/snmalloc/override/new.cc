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

namespace snmalloc
{
  void* failure_throw(size_t size);
  void* failure_nothrow(size_t size);

  template<bool ShouldThrow = true>
  class SetHandlerContinuations
  {
  public:
    static void* success(void* p, size_t size, bool secondary_allocator = false)
    {
      UNUSED(secondary_allocator, size);
      SNMALLOC_ASSERT(p != nullptr);

      SNMALLOC_ASSERT(
        secondary_allocator ||
        is_start_of_object(size_to_sizeclass_full(size), address_cast(p)));

      return p;
    }

    static void* failure(size_t size)
    {
      if constexpr (ShouldThrow)
      {
        // Throw std::bad_alloc on failure.
        return failure_throw(size);
      }
      else
      {
        // Return nullptr on failure.
        return failure_nothrow(size);
      }
    }
  };

  using NoThrow = SetHandlerContinuations<false>;
  using Throw = SetHandlerContinuations<true>;

  void* alloc_nothrow(size_t size)
  {
    return alloc<NoThrow>(size);
  }

  void* alloc_throw(size_t size)
  {
    return alloc<Throw>(size);
  }
} // namespace snmalloc

void* operator new(size_t size)
{
  return snmalloc::alloc<snmalloc::Throw>(size);
}

void* operator new[](size_t size)
{
  return snmalloc::alloc<snmalloc::Throw>(size);
}

void* operator new(size_t size, std::nothrow_t&)
{
  return snmalloc::alloc<snmalloc::NoThrow>(size);
}

void* operator new[](size_t size, std::nothrow_t&)
{
  return snmalloc::alloc<snmalloc::NoThrow>(size);
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
  return snmalloc::alloc<snmalloc::Throw>(size);
}

void* operator new[](size_t size, std::align_val_t val)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::alloc<snmalloc::Throw>(size);
}

void* operator new(size_t size, std::align_val_t val, std::nothrow_t&)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::alloc<snmalloc::NoThrow>(size);
}

void* operator new[](size_t size, std::align_val_t val, std::nothrow_t&)
{
  size = snmalloc::aligned_size(size_t(val), size);
  return snmalloc::alloc<snmalloc::NoThrow>(size);
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
