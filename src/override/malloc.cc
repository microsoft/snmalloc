#include "override.h"

#include <errno.h>
#include <string.h>

using namespace snmalloc;

#ifndef MALLOC_USABLE_SIZE_QUALIFIER
#  define MALLOC_USABLE_SIZE_QUALIFIER
#endif

namespace
{
  /**
   * Helper for JEMalloc-compatible non-standard APIs.  These take a flags
   * argument as an `int`.  This class provides a wrapper for extracting the
   * fields embedded in this API.
   */
  class JEMallocFlags
  {
    /**
     * The raw flags.
     */
    int flags;

  public:
    /**
     * Constructor, takes a `flags` parameter from one of the `*allocx()`
     * JEMalloc APIs.
     */
    constexpr JEMallocFlags(int flags) : flags(flags) {}

    /**
     * Jemalloc's *allocx APIs store the alignment in the low 6 bits of the
     * flags, allowing any alignment up to 2^63.
     */
    constexpr int log2align()
    {
      return flags & 0x3f;
    }

    /**
     * Jemalloc's *allocx APIs use bit 6 to indicate whether memory should be
     * zeroed.
     */
    constexpr bool should_zero()
    {
      return (flags & 0x40) == 0x40;
    }

    /**
     * Jemalloc's *allocm APIs use bit 7 to indicate whether reallocation may
     * move.  This is ignored by the `*allocx` functions.
     */
    constexpr bool may_not_move()
    {
      return (flags & 0x80) == 0x80;
    }

    size_t aligned_size(size_t size)
    {
      return ::aligned_size(bits::one_at_bit<size_t>(log2align()), size);
    }
  };

  /**
   * Error codes from Jemalloc 3's experimental API.
   */
  enum JEMalloc3Result
  {
    /**
     * Allocation succeeded.
     */
    allocm_success = 0,

    /**
     * Allocation failed because memory was not available.
     */
    allocm_err_oom = 1,

    /**
     * Reallocation failed because it would have required moving.
     */
    allocm_err_not_moved = 2
  };
} // namespace

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
    if (SNMALLOC_UNLIKELY(overflow))
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
    if (SNMALLOC_LIKELY(p != nullptr))
    {
      sz = bits::min(size, sz);
      // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
      // otherwise.
      if (sz != 0)
        memcpy(p, ptr, sz);
      a.dealloc(ptr);
    }
    else if (SNMALLOC_LIKELY(size == 0))
    {
      a.dealloc(ptr);
    }
    return p;
  }

#if !defined(SNMALLOC_NO_REALLOCARRAY)
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

#if !defined(SNMALLOC_NO_REALLOCARR)
  SNMALLOC_EXPORT int
    SNMALLOC_NAME_MANGLE(reallocarr)(void* ptr_, size_t nmemb, size_t size)
  {
    int err = errno;
    auto& a = ThreadAlloc::get();
    bool overflow = false;
    size_t sz = bits::umul(size, nmemb, overflow);
    if (sz == 0)
    {
      errno = err;
      return 0;
    }
    if (overflow)
    {
      errno = err;
      return EOVERFLOW;
    }

    void** ptr = reinterpret_cast<void**>(ptr_);
    void* p = a.alloc(sz);
    if (p == nullptr)
    {
      errno = ENOMEM;
      return ENOMEM;
    }

    sz = bits::min(sz, a.alloc_size(*ptr));
    // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
    // otherwise.
    if (sz != 0)
      memcpy(p, *ptr, sz);
    errno = err;
    a.dealloc(*ptr);
    *ptr = p;
    return 0;
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
    if (SNMALLOC_UNLIKELY(p == nullptr))
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

  SNMALLOC_EXPORT SNMALLOC_USED_FUNCTION inline void _malloc_prefork(void) {}
  SNMALLOC_EXPORT SNMALLOC_USED_FUNCTION inline void _malloc_postfork(void) {}
  SNMALLOC_EXPORT SNMALLOC_USED_FUNCTION inline void _malloc_first_thread(void)
  {}

  /**
   * Jemalloc API provides a way of avoiding name lookup when calling
   * `mallctl`.  For now, always return an error.
   */
  int SNMALLOC_NAME_MANGLE(mallctlnametomib)(const char*, size_t*, size_t*)
  {
    return ENOENT;
  }

  /**
   * Jemalloc API provides a generic entry point for various functions.  For
   * now, this is always implemented to return an error.
   */
  int SNMALLOC_NAME_MANGLE(mallctlbymib)(
    const size_t*, size_t, void*, size_t*, void*, size_t)
  {
    return ENOENT;
  }

  /**
   * Jemalloc API provides a generic entry point for various functions.  For
   * now, this is always implemented to return an error.
   */
  SNMALLOC_EXPORT int
    SNMALLOC_NAME_MANGLE(mallctl)(const char*, void*, size_t*, void*, size_t)
  {
    return ENOENT;
  }

  /**
   * Jemalloc 3 experimental API.  Allocates  at least `size` bytes and returns
   * the result in `*ptr`, if `rsize` is not null then writes the allocated size
   * into `*rsize`.  `flags` controls whether the memory is zeroed and what
   * alignment is requested.
   */
  int SNMALLOC_NAME_MANGLE(allocm)(
    void** ptr, size_t* rsize, size_t size, int flags)
  {
    auto f = JEMallocFlags(flags);
    size = f.aligned_size(size);
    if (rsize != nullptr)
    {
      *rsize = size;
    }
    if (f.should_zero())
    {
      *ptr = ThreadAlloc::get().alloc<ZeroMem::YesZero>(size);
    }
    else
    {
      *ptr = ThreadAlloc::get().alloc(size);
    }
    return (*ptr != nullptr) ? allocm_success : allocm_err_oom;
  }

  /**
   * Jemalloc 3 experimental API.  Reallocates the allocation in `*ptr` to be at
   * least `size` bytes and returns the result in `*ptr`, if `rsize` is not null
   * then writes the allocated size into `*rsize`.  `flags` controls whether the
   * memory is zeroed and what alignment is requested and whether reallocation
   * is permitted.  If reallocating, the size will be at least `size` + `extra`
   * bytes.
   */
  int SNMALLOC_NAME_MANGLE(rallocm)(
    void** ptr, size_t* rsize, size_t size, size_t extra, int flags)
  {
    auto f = JEMallocFlags(flags);
    auto alloc_size = f.aligned_size(size);

    auto& a = ThreadAlloc::get();
    size_t sz = a.alloc_size(*ptr);
    // Keep the current allocation if the given size is in the same sizeclass.
    if (sz == round_size(alloc_size))
    {
      if (rsize != nullptr)
      {
        *rsize = sz;
      }
      return allocm_success;
    }

    if (f.may_not_move())
    {
      return allocm_err_not_moved;
    }

    if (std::numeric_limits<size_t>::max() - size > extra)
    {
      alloc_size = f.aligned_size(size + extra);
    }

    void* p =
      f.should_zero() ? a.alloc<YesZero>(alloc_size) : a.alloc(alloc_size);
    if (SNMALLOC_LIKELY(p != nullptr))
    {
      sz = bits::min(alloc_size, sz);
      // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
      // otherwise.
      if (sz != 0)
      {
        memcpy(p, *ptr, sz);
      }
      a.dealloc(*ptr);
      *ptr = p;
      if (rsize != nullptr)
      {
        *rsize = size;
      }
      return allocm_success;
    }
    return allocm_err_oom;
  }

  /**
   * Jemalloc 3 experimental API.  Sets `*rsize` to the size of the allocation
   * at `*ptr`.  The third argument contains some flags relating to arenas that
   * we ignore.
   */
  int SNMALLOC_NAME_MANGLE(sallocm)(const void* ptr, size_t* rsize, int)
  {
    auto& a = ThreadAlloc::get();
    *rsize = a.alloc_size(ptr);
    return allocm_success;
  }

  /**
   * Jemalloc 3 experimental API.  Deallocates the allocation
   * at `*ptr`.  The second argument contains some flags relating to arenas that
   * we ignore.
   */
  int SNMALLOC_NAME_MANGLE(dallocm)(void* ptr, int)
  {
    ThreadAlloc::get().dealloc(ptr);
    return allocm_success;
  }

  /**
   * Jemalloc 3 experimental API.  Returns in `*rsize` the size of the
   * allocation that would be returned if `size` and `flags` are passed to
   * `allocm`.
   */
  int SNMALLOC_NAME_MANGLE(nallocm)(size_t* rsize, size_t size, int flags)
  {
    *rsize = round_size(JEMallocFlags(flags).aligned_size(size));
    return allocm_success;
  }

  /**
   * Jemalloc function that provides control over alignment and zeroing
   * behaviour via the `flags` argument.  This argument also includes control
   * over the thread cache and arena to use.  These don't translate directly to
   * snmalloc and so are ignored.
   */
  SNMALLOC_EXPORT void* SNMALLOC_NAME_MANGLE(mallocx)(size_t size, int flags)
  {
    auto f = JEMallocFlags(flags);
    size = f.aligned_size(size);
    if (f.should_zero())
    {
      return ThreadAlloc::get().alloc<ZeroMem::YesZero>(size);
    }
    return ThreadAlloc::get().alloc(size);
  }

  /**
   * Jemalloc non-standard function that is similar to `realloc`.  This can
   * request zeroed memory for any newly allocated memory, though only if the
   * object grows (which, for snmalloc, means if it's copied).  The flags
   * controlling the thread cache and arena are ignored.
   */
  SNMALLOC_EXPORT void*
    SNMALLOC_NAME_MANGLE(rallocx)(void* ptr, size_t size, int flags)
  {
    auto f = JEMallocFlags(flags);
    size = f.aligned_size(size);

    auto& a = ThreadAlloc::get();
    size_t sz = a.alloc_size(ptr);
    // Keep the current allocation if the given size is in the same sizeclass.
    if (sz == round_size(size))
    {
      return ptr;
    }

    if (size == (size_t)-1)
    {
      return nullptr;
    }

    void* p = f.should_zero() ? a.alloc<YesZero>(size) : a.alloc(size);
    if (SNMALLOC_LIKELY(p != nullptr))
    {
      sz = bits::min(size, sz);
      // Guard memcpy as GCC is assuming not nullptr for ptr after the memcpy
      // otherwise.
      if (sz != 0)
        memcpy(p, ptr, sz);
      a.dealloc(ptr);
    }
    return p;
  }

  /**
   * Jemalloc non-standard API that performs a `realloc` only if it can do so
   * without copying and returns the size of the underlying object.  With
   * snmalloc, this simply returns the size of the sizeclass backing the
   * object.
   */
  size_t SNMALLOC_NAME_MANGLE(xallocx)(void* ptr, size_t, size_t, int)
  {
    auto& a = ThreadAlloc::get();
    return a.alloc_size(ptr);
  }

  /**
   * Jemalloc non-standard API that queries the underlying size of the
   * allocation.
   */
  size_t SNMALLOC_NAME_MANGLE(sallocx)(const void* ptr, int)
  {
    auto& a = ThreadAlloc::get();
    return a.alloc_size(ptr);
  }

  /**
   * Jemalloc non-standard API that frees `ptr`.  The second argument allows
   * specifying a thread cache or arena but this is currently unused in
   * snmalloc.
   */
  void SNMALLOC_NAME_MANGLE(dallocx)(void* ptr, int)
  {
    ThreadAlloc::get().dealloc(ptr);
  }

  /**
   * Jemalloc non-standard API that frees `ptr`.  The second argument specifies
   * a size, which is intended to speed up the operation.  This could improve
   * performance for snmalloc, if we could guarantee that this is allocated by
   * the current thread but is otherwise not helpful.  The third argument allows
   * specifying a thread cache or arena but this is currently unused in
   * snmalloc.
   */
  void SNMALLOC_NAME_MANGLE(sdallocx)(void* ptr, size_t, int)
  {
    ThreadAlloc::get().dealloc(ptr);
  }

  /**
   * Jemalloc non-standard API that returns the size of memory that would be
   * allocated if the same arguments were passed to `mallocx`.
   */
  size_t SNMALLOC_NAME_MANGLE(nallocx)(size_t size, int flags)
  {
    return round_size(JEMallocFlags(flags).aligned_size(size));
  }

#if !defined(__PIC__) && defined(SNMALLOC_BOOTSTRAP_ALLOCATOR)
  // The following functions are required to work before TLS is set up, in
  // statically-linked programs.  These temporarily grab an allocator from the
  // pool and return it.

  void* __je_bootstrap_malloc(size_t size)
  {
    return get_scoped_allocator()->alloc(size);
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
    return get_scoped_allocator()->alloc<ZeroMem::YesZero>(sz);
  }

  void __je_bootstrap_free(void* ptr)
  {
    get_scoped_allocator()->dealloc(ptr);
  }
#endif
}
