// Core implementation of snmalloc independent of the configuration mode
#include "../snmalloc_core.h"

#ifndef SNMALLOC_PROVIDE_OWN_CONFIG
// The default configuration for snmalloc is used if alternative not defined
namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::Globals>;
} // namespace snmalloc
#endif

// User facing API surface, needs to know what `Alloc` is.
#include "../snmalloc_front.h"

#include <errno.h>
#include <string.h>

using namespace snmalloc;

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif
#ifdef SNMALLOC_STATIC_LIBRARY_PREFIX
#  define __SN_CONCAT(a, b) a##b
#  define __SN_EVALUATE(a, b) __SN_CONCAT(a, b)
#  define SNMALLOC_NAME_MANGLE(a) \
    __SN_EVALUATE(SNMALLOC_STATIC_LIBRARY_PREFIX, a)
#elif !defined(SNMALLOC_NAME_MANGLE)
#  define SNMALLOC_NAME_MANGLE(a) a
#endif

#ifndef MALLOC_USABLE_SIZE_QUALIFIER
#  define MALLOC_USABLE_SIZE_QUALIFIER
#endif

extern "C"
{
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(__malloc_end_pointer)(void* ptr)
  {
    return ThreadAlloc::get().external_pointer<OnePastEnd>(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(malloc)(size_t size)
  {
    return ThreadAlloc::get().alloc(size);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(free)(void* ptr)
  {
    ThreadAlloc::get().dealloc(ptr);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(cfree)(void* ptr)
  {
    ThreadAlloc::get().dealloc(ptr);
  }

  /**
   * Clang was helpfully inlining the constant return value, and
   * thus converting from a tail call to an ordinary call.
   */
  SNMALLOC_EXPORT inline void* snmalloc_not_allocated = nullptr;

  static SNMALLOC_SLOW_PATH void* SNMALLOC_NAME_MANGLE(snmalloc_set_error)()
  {
    errno = ENOMEM;
    return snmalloc_not_allocated;
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(calloc)(size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (unlikely(overflow))
    {
      return SNMALLOC_NAME_MANGLE(snmalloc_set_error)();
    }
    return ThreadAlloc::get().alloc<ZeroMem::YesZero>(sz);
  }

  SNMALLOC_EXPORT
  size_t SNMALLOC_NAME_MANGLE(malloc_usable_size)(
    MALLOC_USABLE_SIZE_QUALIFIER void* ptr)
  {
    return ThreadAlloc::get().alloc_size(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(realloc)(void* ptr, size_t size)
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

    if (size == (size_t)-1)
    {
      errno = ENOMEM;
      return nullptr;
    }

    void* p = a.alloc(size);
    if (likely(p != nullptr))
    {
      sz = bits::min(size, sz);
      // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
      // otherwise.
      if (sz != 0)
        memcpy(p, ptr, sz);
      a.dealloc(ptr);
    }
    else if (likely(size == 0))
    {
      a.dealloc(ptr);
    }
    return p;
  }

#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(reallocarray)(void* ptr, size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (overflow)
    {
      errno = ENOMEM;
      return nullptr;
    }
    return SNMALLOC_NAME_MANGLE(realloc)(ptr, sz);
  }
#endif

  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(memalign)(size_t alignment, size_t size)
  {
    if ((alignment == 0) || (alignment == size_t(-1)))
    {
      errno = EINVAL;
      return nullptr;
    }

    if ((size + alignment) < size)
    {
      errno = ENOMEM;
      return nullptr;
    }

    return SNMALLOC_NAME_MANGLE(malloc)(aligned_size(alignment, size));
  }

  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(aligned_alloc)(size_t alignment, size_t size)
  {
    SNMALLOC_ASSERT((size % alignment) == 0);
    return SNMALLOC_NAME_MANGLE(memalign)(alignment, size);
  }

  SNMALLOC_EXPORT int SNMALLOC_NAME_MANGLE(posix_memalign)(
    void** memptr, size_t alignment, size_t size)
  {
    if ((alignment < sizeof(uintptr_t) || ((alignment & (alignment - 1)) != 0)))
    {
      return EINVAL;
    }

    void* p = SNMALLOC_NAME_MANGLE(memalign)(alignment, size);
    if (unlikely(p == nullptr))
    {
      if (size != 0)
        return ENOMEM;
    }
    *memptr = p;
    return 0;
  }

#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(valloc)(size_t size)
  {
    return SNMALLOC_NAME_MANGLE(memalign)(OS_PAGE_SIZE, size);
  }
#endif

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(pvalloc)(size_t size)
  {
    if (size == size_t(-1))
    {
      errno = ENOMEM;
      return nullptr;
    }
    return SNMALLOC_NAME_MANGLE(memalign)(
      OS_PAGE_SIZE, (size + OS_PAGE_SIZE - 1) & ~(OS_PAGE_SIZE - 1));
  }

  // Stub implementations for jemalloc compatibility.
  // These are called by FreeBSD's libthr (pthreads) to notify malloc of
  // various events.  They are currently unused, though we may wish to reset
  // statistics on fork if built with statistics.

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(_malloc_prefork)(void) {}
  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(_malloc_postfork)(void) {}
  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(_malloc_first_thread)(void) {}

  SNMALLOC_EXPORT int
    SNMALLOC_NAME_MANGLE(mallctl)(const char*, void*, size_t*, void*, size_t)
  {
    return ENOENT;
  }

#if !defined(__PIC__) && defined(SNMALLOC_BOOTSTRAP_ALLOCATOR)
  // The following functions are required to work before TLS is set up, in
  // statically-linked programs.  These temporarily grab an allocator from the
  // pool and return it.

  void* __je_bootstrap_malloc(size_t size)
  {
    return get_scoped_allocator().alloc(size);
  }

  void* __je_bootstrap_calloc(size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (overflow)
    {
      errno = ENOMEM;
      return nullptr;
    }
    // Include size 0 in the first sizeclass.
    sz = ((sz - 1) >> (bits::BITS - 1)) + sz;
    return get_scoped_allocator().alloc<ZeroMem::YesZero>(sz);
  }

  void __je_bootstrap_free(void* ptr)
  {
    get_scoped_allocator().dealloc(ptr);
  }
#endif
}
