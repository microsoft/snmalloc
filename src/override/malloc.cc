#include "../mem/slowalloc.h"
#include "../snmalloc.h"

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
  void SNMALLOC_NAME_MANGLE(check_start)(void* ptr)
  {
#if !defined(NDEBUG) && !defined(SNMALLOC_PASS_THROUGH)
    if (Alloc::external_pointer<Start>(ptr) != ptr)
    {
      error("Using pointer that is not to the start of an allocation");
    }
#else
    UNUSED(ptr);
#endif
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(__malloc_end_pointer)(void* ptr)
  {
    return Alloc::external_pointer<OnePastEnd>(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(malloc)(size_t size)
  {
    return ThreadAlloc::get_noncachable()->alloc(size);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(free)(void* ptr)
  {
    SNMALLOC_NAME_MANGLE(check_start)(ptr);
    ThreadAlloc::get_noncachable()->dealloc(ptr);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(cfree)(void* ptr)
  {
    SNMALLOC_NAME_MANGLE(free)(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(calloc)(size_t nmemb, size_t size)
  {
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (overflow)
    {
      errno = ENOMEM;
      return nullptr;
    }
    return ThreadAlloc::get_noncachable()->alloc<ZeroMem::YesZero>(sz);
  }

  SNMALLOC_EXPORT
  size_t SNMALLOC_NAME_MANGLE(malloc_usable_size)(
    MALLOC_USABLE_SIZE_QUALIFIER void* ptr)
  {
    return Alloc::alloc_size(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(realloc)(void* ptr, size_t size)
  {
    if (size == (size_t)-1)
    {
      errno = ENOMEM;
      return nullptr;
    }
    if (ptr == nullptr)
    {
      return SNMALLOC_NAME_MANGLE(malloc)(size);
    }
    if (size == 0)
    {
      SNMALLOC_NAME_MANGLE(free)(ptr);
      return nullptr;
    }

    SNMALLOC_NAME_MANGLE(check_start)(ptr);

    size_t sz = Alloc::alloc_size(ptr);
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
    void* p = SNMALLOC_NAME_MANGLE(malloc)(size);
    if (p != nullptr)
    {
      SNMALLOC_NAME_MANGLE(check_start)(p);
      sz = bits::min(size, sz);
      memcpy(p, ptr, sz);
      SNMALLOC_NAME_MANGLE(free)(ptr);
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

    return SNMALLOC_NAME_MANGLE(malloc)(
      size ? aligned_size(alignment, size) : alignment);
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
    if (
      ((alignment % sizeof(uintptr_t)) != 0) ||
      ((alignment & (alignment - 1)) != 0) || (alignment == 0))
    {
      return EINVAL;
    }

    void* p = SNMALLOC_NAME_MANGLE(memalign)(alignment, size);
    if (p == nullptr)
    {
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

#ifdef SNMALLOC_EXPOSE_PAGEMAP
  /**
   * Export the pagemap.  The return value is a pointer to the pagemap
   * structure.  The argument is used to return a pointer to a `PagemapConfig`
   * structure describing the type of the pagemap.  Static methods on the
   * concrete pagemap templates can then be used to safely cast the return from
   * this function to the correct type.  This allows us to preserve some
   * semblance of ABI safety via a pure C API.
   */
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(snmalloc_pagemap_global_get)(
    PagemapConfig const** config)
  {
    auto& pm = GlobalPagemap::pagemap();
    if (config)
    {
      *config = &ChunkmapPagemap::config;
      SNMALLOC_ASSERT(ChunkmapPagemap::cast_to_pagemap(&pm, *config) == &pm);
    }
    return &pm;
  }
#endif

#ifdef SNMALLOC_EXPOSE_RESERVE
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(snmalloc_reserve_shared)(size_t* size, size_t align)
  {
    return snmalloc::default_memory_provider().reserve<true>(size, align);
  }
#endif

#if !defined(__PIC__) && !defined(NO_BOOTSTRAP_ALLOCATOR)
  // The following functions are required to work before TLS is set up, in
  // statically-linked programs.  These temporarily grab an allocator from the
  // pool and return it.

  void* __je_bootstrap_malloc(size_t size)
  {
    return get_slow_allocator()->alloc(size);
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
    return get_slow_allocator()->alloc<ZeroMem::YesZero>(sz);
  }

  void __je_bootstrap_free(void* ptr)
  {
    get_slow_allocator()->dealloc(ptr);
  }
#endif
}
