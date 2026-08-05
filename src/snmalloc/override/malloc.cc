#include "override.h"

using namespace snmalloc;

#ifndef MALLOC_USABLE_SIZE_QUALIFIER
#  define MALLOC_USABLE_SIZE_QUALIFIER
#endif

#ifdef _WIN32
#  include <cstring>

namespace
{
  inline void win_set_exact_size(void* ptr, size_t req_size)
  {
    if (!ptr)
      return;
      
    auto* alloc = snmalloc::ThreadAlloc::get();
    size_t alloc_size = alloc->alloc_size(ptr);
    size_t diff = alloc_size - req_size;
    
    // We have 1 byte to store the difference (max 255 bytes of padding).
    if (diff > 0 && diff <= 255)
    {
      alloc->set_client_meta_data<bool>(ptr, true);
      static_cast<uint8_t*>(ptr)[alloc_size - 1] = static_cast<uint8_t>(diff);
    }
    else
    {
      alloc->set_client_meta_data<bool>(ptr, false);
    }
  }

  inline size_t win_get_exact_size(void* ptr)
  {
    if (!ptr)
      return 0;
      
    auto* alloc = snmalloc::ThreadAlloc::get();
    size_t alloc_size = alloc->alloc_size(ptr);
    bool has_padding = alloc->get_client_meta_data<bool>(ptr);
    
    if (has_padding)
    {
      uint8_t unused = static_cast<uint8_t*>(ptr)[alloc_size - 1];
      return alloc_size - unused;
    }
    
    return alloc_size;
  }
} // namespace
#endif

extern "C"
{
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(__malloc_end_pointer)(void* ptr)
  {
    return snmalloc::libc::__malloc_end_pointer(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(malloc)(size_t size)
  {
    void* ptr = snmalloc::libc::malloc(size);
#ifdef _WIN32
    win_set_exact_size(ptr, size);
#endif
    return ptr;
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(free)(void* ptr)
  {
    snmalloc::libc::free(ptr);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(free_sized)(void* ptr, size_t size)
  {
    snmalloc::libc::free_sized(ptr, size);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(free_aligned_sized)(
    void* ptr, size_t alignment, size_t size)
  {
    snmalloc::libc::free_aligned_sized(ptr, alignment, size);
  }

  SNMALLOC_EXPORT void SNMALLOC_NAME_MANGLE(cfree)(void* ptr)
  {
    snmalloc::libc::free(ptr);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(calloc)(size_t nmemb, size_t size)
  {
    void* ptr = snmalloc::libc::calloc(nmemb, size);
#ifdef _WIN32
    win_set_exact_size(ptr, nmemb * size);
#endif
    return ptr;
  }

  SNMALLOC_EXPORT
  size_t SNMALLOC_NAME_MANGLE(malloc_usable_size)(
    MALLOC_USABLE_SIZE_QUALIFIER void* ptr)
  {
    return snmalloc::libc::malloc_usable_size(ptr);
  }

#ifdef _WIN32
  SNMALLOC_EXPORT
  size_t SNMALLOC_NAME_MANGLE(_msize)(void* ptr)
  {
    return win_get_exact_size(ptr);
  }

  SNMALLOC_EXPORT
  void* SNMALLOC_NAME_MANGLE(_recalloc)(void* ptr, size_t num, size_t size)
  {
    size_t req_size = num * size;
    if (!ptr)
    {
      return SNMALLOC_NAME_MANGLE(calloc)(num, size);
    }

    size_t old_exact_size = win_get_exact_size(ptr);
    void* new_ptr = SNMALLOC_NAME_MANGLE(malloc)(req_size);

    if (new_ptr)
    {
      size_t copy_size = (old_exact_size < req_size) ? old_exact_size : req_size;
      std::memcpy(new_ptr, ptr, copy_size);

      if (req_size > old_exact_size)
      {
        std::memset(
          static_cast<uint8_t*>(new_ptr) + old_exact_size,
          0,
          req_size - old_exact_size);
      }

      SNMALLOC_NAME_MANGLE(free)(ptr);
    }
    return new_ptr;
  }
#endif

  SNMALLOC_EXPORT
  size_t SNMALLOC_NAME_MANGLE(malloc_good_size)(size_t size)
  {
    return round_size(size);
  }

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(realloc)(void* ptr, size_t size)
  {
    void* new_ptr = snmalloc::libc::realloc(ptr, size);
#ifdef _WIN32
    win_set_exact_size(new_ptr, size);
#endif
    return new_ptr;
  }

#if !defined(SNMALLOC_NO_REALLOCARRAY)
  SNMALLOC_EXPORT void*
  SNMALLOC_NAME_MANGLE(reallocarray)(void* ptr, size_t nmemb, size_t size)
  {
    void* new_ptr = snmalloc::libc::reallocarray(ptr, nmemb, size);
#ifdef _WIN32
    win_set_exact_size(new_ptr, nmemb * size);
#endif
    return new_ptr;
  }
#endif

#if !defined(SNMALLOC_NO_REALLOCARR)
  SNMALLOC_EXPORT int
  SNMALLOC_NAME_MANGLE(reallocarr)(void* ptr, size_t nmemb, size_t size)
  {
    int ret = snmalloc::libc::reallocarr(ptr, nmemb, size);
#ifdef _WIN32
    if (ret == 0 && ptr != nullptr) 
    {
      void* p = *static_cast<void**>(ptr);
      win_set_exact_size(p, nmemb * size);
    }
#endif
    return ret;
  }
#endif

  SNMALLOC_EXPORT void*
  SNMALLOC_NAME_MANGLE(memalign)(size_t alignment, size_t size)
  {
    void* ptr = snmalloc::libc::memalign(alignment, size);
#ifdef _WIN32
    win_set_exact_size(ptr, size);
#endif
    return ptr;
  }

  SNMALLOC_EXPORT void*
  SNMALLOC_NAME_MANGLE(aligned_alloc)(size_t alignment, size_t size)
  {
    void* ptr = snmalloc::libc::aligned_alloc(alignment, size);
#ifdef _WIN32
    win_set_exact_size(ptr, size);
#endif
    return ptr;
  }

  SNMALLOC_EXPORT int SNMALLOC_NAME_MANGLE(posix_memalign)(
    void** memptr, size_t alignment, size_t size)
  {
    int ret = snmalloc::libc::posix_memalign(memptr, alignment, size);
#ifdef _WIN32
    if (ret == 0 && memptr != nullptr) 
    {
      win_set_exact_size(*memptr, size);
    }
#endif
    return ret;
  }

#if !defined(__FreeBSD__) && !defined(__OpenBSD__)
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(valloc)(size_t size)
  {
    void* ptr = snmalloc::libc::memalign(OS_PAGE_SIZE, size);
#ifdef _WIN32
    win_set_exact_size(ptr, size);
#endif
    return ptr;
  }
#endif

  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(pvalloc)(size_t size)
  {
    size_t rounded_size = (size + OS_PAGE_SIZE - 1) & ~(OS_PAGE_SIZE - 1);
    void* ptr = snmalloc::libc::memalign(OS_PAGE_SIZE, rounded_size);
#ifdef _WIN32
    win_set_exact_size(ptr, rounded_size);
#endif
    return ptr;
  }

#if __has_include(<features.h>)
#  include <features.h>
#endif
#if defined(__GLIBC__)
  // glibc uses these hooks to replace malloc.
  // This is required when RTL_DEEPBIND is used and the library is
  // LD_PRELOADed.
  // See https://github.com/microsoft/snmalloc/issues/595
  SNMALLOC_EXPORT void (*SNMALLOC_NAME_MANGLE(__free_hook))(void* ptr) =
    &SNMALLOC_NAME_MANGLE(free);
  SNMALLOC_EXPORT void* (*SNMALLOC_NAME_MANGLE(__malloc_hook))(size_t size) =
    &SNMALLOC_NAME_MANGLE(malloc);
  SNMALLOC_EXPORT void* (*SNMALLOC_NAME_MANGLE(__realloc_hook))(
    void* ptr, size_t size) = &SNMALLOC_NAME_MANGLE(realloc);
  SNMALLOC_EXPORT void* (*SNMALLOC_NAME_MANGLE(__memalign_hook))(
    size_t alignment, size_t size) = &SNMALLOC_NAME_MANGLE(memalign);
#endif
}
