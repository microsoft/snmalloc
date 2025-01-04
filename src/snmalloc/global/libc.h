#pragma once

#include "threadalloc.h"

#include <errno.h>
#include <string.h>

namespace snmalloc::libc
{
  SNMALLOC_SLOW_PATH inline void* set_error(int err = ENOMEM)
  {
    errno = err;
    return nullptr;
  }

  SNMALLOC_SLOW_PATH inline int set_error_and_return(int err = ENOMEM)
  {
    errno = err;
    return err;
  }

  inline void* __malloc_end_pointer(void* ptr)
  {
    return ThreadAlloc::get().external_pointer<OnePastEnd>(ptr);
  }

  SNMALLOC_FAST_PATH_INLINE void* malloc(size_t size)
  {
    return ThreadAlloc::get().alloc(size);
  }

  SNMALLOC_FAST_PATH_INLINE void free(void* ptr)
  {
    ThreadAlloc::get().dealloc(ptr);
  }

  SNMALLOC_FAST_PATH_INLINE void free_sized(void* ptr, size_t size)
  {
    ThreadAlloc::get().dealloc(ptr, size);
  }

  SNMALLOC_FAST_PATH_INLINE void* calloc(size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (SNMALLOC_UNLIKELY(overflow))
    {
      return set_error();
    }
    return ThreadAlloc::get().alloc<ZeroMem::YesZero>(sz);
  }

  SNMALLOC_FAST_PATH_INLINE void* realloc(void* ptr, size_t size)
  {
    auto& a = ThreadAlloc::get();
    size_t sz = a.alloc_size(ptr);
    // Keep the current allocation if the given size is in the same sizeclass.
    if (sz == round_size(size))
    {
#ifdef SNMALLOC_PASS_THROUGH
      // snmallocs alignment guarantees can be broken by realloc in pass-through
      // this is not exercised, by existing clients, but is tested.
      if (pointer_align_up(ptr, natural_alignment(size)) == ptr)
        return ptr;
#else
      return ptr;
#endif
    }

    void* p = a.alloc(size);
    if (SNMALLOC_LIKELY(p != nullptr))
    {
      sz = bits::min(size, sz);
      // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
      // otherwise.
      if (SNMALLOC_UNLIKELY(sz != 0))
      {
        SNMALLOC_ASSUME(ptr != nullptr);
        ::memcpy(p, ptr, sz);
      }
      a.dealloc(ptr);
    }
    else if (SNMALLOC_LIKELY(size == 0))
    {
      a.dealloc(ptr);
    }
    else
    {
      return set_error();
    }
    return p;
  }

  inline size_t malloc_usable_size(const void* ptr)
  {
    return ThreadAlloc::get().alloc_size(ptr);
  }

  inline void* reallocarray(void* ptr, size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (SNMALLOC_UNLIKELY(overflow))
    {
      return set_error();
    }
    return realloc(ptr, sz);
  }

  inline int reallocarr(void* ptr_, size_t nmemb, size_t size)
  {
    int err = errno;
    auto& a = ThreadAlloc::get();
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (SNMALLOC_UNLIKELY(sz == 0))
    {
      errno = err;
      return 0;
    }
    if (SNMALLOC_UNLIKELY(overflow))
    {
      return set_error_and_return(EOVERFLOW);
    }

    void** ptr = reinterpret_cast<void**>(ptr_);
    void* p = a.alloc(sz);
    if (SNMALLOC_UNLIKELY(p == nullptr))
    {
      return set_error_and_return(ENOMEM);
    }

    sz = bits::min(sz, a.alloc_size(*ptr));

    SNMALLOC_ASSUME(*ptr != nullptr || sz == 0);
    // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
    // otherwise.
    if (SNMALLOC_UNLIKELY(sz != 0))
      ::memcpy(p, *ptr, sz);
    errno = err;
    a.dealloc(*ptr);
    *ptr = p;
    return 0;
  }

  inline void* memalign(size_t alignment, size_t size)
  {
    if (SNMALLOC_UNLIKELY(alignment == 0 || !bits::is_pow2(alignment)))
    {
      return set_error(EINVAL);
    }

    return malloc(aligned_size(alignment, size));
  }

  inline void* aligned_alloc(size_t alignment, size_t size)
  {
    return memalign(alignment, size);
  }

  inline int posix_memalign(void** memptr, size_t alignment, size_t size)
  {
    if (SNMALLOC_UNLIKELY(
          (alignment < sizeof(uintptr_t) || !bits::is_pow2(alignment))))
    {
      return EINVAL;
    }

    void* p = memalign(alignment, size);
    if (SNMALLOC_UNLIKELY(p == nullptr))
    {
      if (size != 0)
        return ENOMEM;
    }
    *memptr = p;
    return 0;
  }

  inline typename snmalloc::Alloc::Config::ClientMeta::DataRef
  get_client_meta_data(void* p)
  {
    return ThreadAlloc::get().get_client_meta_data(p);
  }

  inline stl::add_const_t<typename snmalloc::Alloc::Config::ClientMeta::DataRef>
  get_client_meta_data_const(void* p)
  {
    return ThreadAlloc::get().get_client_meta_data_const(p);
  }

} // namespace snmalloc::libc
