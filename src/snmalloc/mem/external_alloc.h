#pragma once

#ifdef SNMALLOC_PASS_THROUGH
#  if defined(__HAIKU__)
#    define _GNU_SOURCE
#  endif
#  include <stdlib.h>
#  if defined(_WIN32) //|| defined(__APPLE__)
#    error "Pass through not supported on this platform"
//   The Windows aligned allocation API is not capable of supporting the
//   snmalloc API Apple was not providing aligned memory in some tests.
#  else
//  Defines malloc_size for the platform.
#    if defined(_WIN32)
namespace snmalloc::external_alloc
{
  inline size_t malloc_usable_size(void* ptr)
  {
    return _msize(ptr);
  }
}
#    elif defined(__APPLE__)
#      include <malloc/malloc.h>

namespace snmalloc::external_alloc
{
  inline size_t malloc_usable_size(void* ptr)
  {
    return malloc_size(ptr);
  }
}
#    elif defined(__linux__) || defined(__HAIKU__)
#      include <malloc.h>

namespace snmalloc::external_alloc
{
  using ::malloc_usable_size;
}
#    elif defined(__sun) || defined(__NetBSD__) || defined(__OpenBSD__)
namespace snmalloc::external_alloc
{
  using ::malloc_usable_size;
}
#    elif defined(__FreeBSD__)
#      include <malloc_np.h>

namespace snmalloc::external_alloc
{
  using ::malloc_usable_size;
}
#    elif defined(__DragonFly__)
namespace snmalloc::external_alloc
{
  using ::malloc_usable_size;
}
#    else
#      error Define malloc size macro for this platform.
#    endif
namespace snmalloc::external_alloc
{
  inline void* aligned_alloc(size_t alignment, size_t size)
  {
    // TSAN complains if allocation is large than this.
    if constexpr (bits::BITS == 64)
    {
      if (size >= 0x10000000000)
      {
        errno = ENOMEM;
        return nullptr;
      }
    }

    if (alignment < sizeof(void*))
      alignment = sizeof(void*);

    void* result;
    int err = posix_memalign(&result, alignment, size);
    if (err != 0)
    {
      errno = err;
      result = nullptr;
    }
    return result;
  }

  using ::free;
}
#  endif
#endif
