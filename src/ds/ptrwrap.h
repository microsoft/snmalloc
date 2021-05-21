#pragma once

#include <atomic>

namespace snmalloc
{
  /**
   * To assist in providing a uniform interface regardless of pointer wrapper,
   * we also export intrinsic pointer and atomic pointer aliases, as the postfix
   * type constructor '*' does not work well as a template parameter and we
   * don't have inline type-level functions.
   */
  template<typename T>
  using Pointer = T*;

  template<typename T>
  using AtomicPointer = std::atomic<T*>;

  /**
   * Summaries of StrictProvenance metadata.  We abstract away the particular
   * size and any offset into the bounds.
   *
   * CBArena is as powerful as our pointers get: they're results from mmap(),
   * and so confer as much authority as the kernel has given us.
   *
   * CBChunk is restricted to either a single chunk (SUPERSLAB_SIZE) or perhaps
   * to several if we've requesed a large allocation (see capptr_chunk_is_alloc
   * and its uses).
   *
   * CBChunkD is curious: we often use CBArena-bounded pointers to derive
   * pointers to Allocslab metadata, and on most fast paths these pointers end
   * up being ephemeral.  As such, on NDEBUG builds, we elide the capptr_bounds
   * that would bound these to chunks and instead just unsafely inherit the
   * CBArena bounds.  The use of CBChunkD thus helps to ensure that we
   * eventually do invoke capptr_bounds when these pointers end up being longer
   * lived!
   *
   * *E forms are "exported" and have had platform constraints applied.  That
   * means, for example, on CheriBSD, that they have had their VMMAP permission
   * stripped.
   *
   * Yes, I wish the start-of-comment characters were aligned below as well.
   * I blame clang format.
   */
  enum capptr_bounds
  {
    /*                Spatial  Notes                                      */
    CBArena, /*        Arena                                              */
    CBChunkD, /*       Arena   Chunk-bounded in debug; internal use only! */
    CBChunk, /*        Chunk                                              */
    CBChunkE, /*       Chunk   (+ platform constraints)                   */
    CBAlloc, /*        Alloc                                              */
    CBAllocE /*        Alloc   (+ platform constraints)                   */
  };

  /**
   * Compute the "exported" variant of a capptr_bounds annotation.  This is
   * used by the PAL's capptr_export function to compute its return value's
   * annotation.
   */
  template<capptr_bounds B>
  SNMALLOC_CONSTEVAL capptr_bounds capptr_export_type()
  {
    static_assert(
      (B == CBChunk) || (B == CBAlloc), "capptr_export_type of bad type");

    switch (B)
    {
      case CBChunk:
        return CBChunkE;
      case CBAlloc:
        return CBAllocE;
    }
  }

  template<capptr_bounds BI, capptr_bounds BO>
  SNMALLOC_CONSTEVAL bool capptr_is_bounds_refinement()
  {
    switch (BI)
    {
      case CBAllocE:
        return BO == CBAllocE;
      case CBAlloc:
        return BO == CBAlloc;
      case CBChunkE:
        return BO == CBAllocE || BO == CBChunkE;
      case CBChunk:
        return BO == CBAlloc || BO == CBChunk || BO == CBChunkD;
      case CBChunkD:
        return BO == CBAlloc || BO == CBChunk || BO == CBChunkD;
      case CBArena:
        return BO == CBAlloc || BO == CBChunk || BO == CBChunkD ||
          BO == CBArena;
    }
  }

  /**
   * A pointer annotated with a "phantom type parameter" carrying a static
   * summary of its StrictProvenance metadata.
   */
  template<typename T, capptr_bounds bounds>
  struct CapPtr
  {
    uintptr_t unsafe_capptr;

    /**
     * nullptr is implicitly constructable at any bounds type.
     *
     * Annoyingly, this isn't possible in constexpr context: the
     * reinterpret_cast isn't allowed.  If you're after the all-zeros CapPtr,
     * use the uintptr_t-consuming constructor below.
     */
    SNMALLOC_FAST_PATH CapPtr(const std::nullptr_t n)
    : unsafe_capptr(reinterpret_cast<uintptr_t>(n))
    {}

    SNMALLOC_FAST_PATH CapPtr() : CapPtr(nullptr) {}

    /**
     * all other constructions must be explicit
     *
     * Unfortunately, MSVC gets confused if an Allocator is instantiated in a
     * way that never needs initialization (as our sandbox test does, for
     * example) and, in that case, declares this constructor unreachable,
     * presumably after some heroic feat of inlining that has also lost any
     * semblance of context.  See the blocks tagged "CapPtr-vs-MSVC" for where
     * this has been observed.
     */
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable : 4702)
#endif
    SNMALLOC_FAST_PATH explicit CapPtr(T* p)
    : unsafe_capptr(reinterpret_cast<uintptr_t>(p))
    {}
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

    /**
     * We permit construction of CapPtr-s from integral values as well.  Notably
     * this works in constexpr.  Because of the special-casing of literal 0, one
     * may need to say "CapPtr<>(static_cast<uintptr_t>(0))" to avoid matching
     * the (non-constexpr) nullptr constructor above.
     */
    SNMALLOC_FAST_PATH constexpr explicit CapPtr(uintptr_t p) : unsafe_capptr(p)
    {}

    /**
     * Allow static_cast<>-s that preserve bounds but vary the target type.
     */
    template<typename U>
    SNMALLOC_FAST_PATH CapPtr<U, bounds> as_static()
    {
      return CapPtr<U, bounds>(reinterpret_cast<U*>(this->unsafe_capptr));
    }

    SNMALLOC_FAST_PATH CapPtr<void, bounds> as_void()
    {
      return this->as_static<void>();
    }

    /**
     * A more aggressive bounds-preserving cast, using reinterpret_cast
     */
    template<typename U>
    SNMALLOC_FAST_PATH CapPtr<U, bounds> as_reinterpret()
    {
      return CapPtr<U, bounds>(reinterpret_cast<U*>(this->unsafe_capptr));
    }

    constexpr SNMALLOC_FAST_PATH bool operator==(const CapPtr& rhs) const
    {
      return this->unsafe_capptr == rhs.unsafe_capptr;
    }

    constexpr SNMALLOC_FAST_PATH bool operator!=(const CapPtr& rhs) const
    {
      return this->unsafe_capptr != rhs.unsafe_capptr;
    }

    constexpr SNMALLOC_FAST_PATH bool operator<(const CapPtr& rhs) const
    {
      return this->unsafe_capptr < rhs.unsafe_capptr;
    }

    [[nodiscard]] SNMALLOC_FAST_PATH T* unsafe_ptr() const
    {
      return reinterpret_cast<T*>(this->unsafe_capptr);
    }

    SNMALLOC_FAST_PATH T* operator->() const
    {
      /*
       * CBAllocE bounds are associated with objects coming from or going to the
       * client; we should be doing nothing with them.
       */
      static_assert(bounds != CBAllocE);
      return this->unsafe_ptr();
    }
  };

  static_assert(sizeof(CapPtr<void, CBArena>) == sizeof(void*));
  static_assert(alignof(CapPtr<void, CBArena>) == alignof(void*));

  template<typename T>
  using CapPtrCBArena = CapPtr<T, CBArena>;

  template<typename T>
  using CapPtrCBChunk = CapPtr<T, CBChunk>;

  template<typename T>
  using CapPtrCBChunkE = CapPtr<T, CBChunkE>;

  template<typename T>
  using CapPtrCBAlloc = CapPtr<T, CBAlloc>;

  /**
   * Sometimes (with large allocations) we really mean the entire chunk (or even
   * several chunks) to be the allocation.
   */
  template<typename T>
  SNMALLOC_FAST_PATH CapPtr<T, CBAllocE>
  capptr_chunk_is_alloc(CapPtr<T, CBChunkE> p)
  {
    return CapPtr<T, CBAlloc>(p.unsafe_ptr());
  }

  /**
   * With all the bounds and constraints in place, it's safe to extract a void
   * pointer (to reveal to the client).
   */
  SNMALLOC_FAST_PATH void* capptr_reveal(CapPtr<void, CBAllocE> p)
  {
    return p.as_void().unsafe_ptr();
  }

  /**
   *
   * Wrap a std::atomic<T*> with bounds annotation and speak in terms of
   * bounds-annotated pointers at the interface.
   *
   * Note the membranous sleight of hand being pulled here: this class puts
   * annotations around an un-annotated std::atomic<T*>, to appease C++, yet
   * will expose or consume only CapPtr<T> with the same bounds annotation.
   */
  template<typename T, capptr_bounds bounds>
  struct AtomicCapPtr
  {
    std::atomic<uintptr_t> unsafe_capptr;

    /**
     * nullptr is constructable at any bounds type
     */
    SNMALLOC_FAST_PATH AtomicCapPtr(const std::nullptr_t n)
    : unsafe_capptr(reinterpret_cast<uintptr_t>(n))
    {}

    /**
     * Interconversion with CapPtr
     */
    SNMALLOC_FAST_PATH AtomicCapPtr(CapPtr<T, bounds> p)
    : unsafe_capptr(p.unsafe_capptr)
    {}

    SNMALLOC_FAST_PATH operator CapPtr<T, bounds>() const noexcept
    {
      return CapPtr<T, bounds>(
        reinterpret_cast<T*>(this->unsafe_capptr.load()));
    }

    // Our copy-assignment operator follows std::atomic and returns a copy of
    // the RHS.  Clang finds this surprising; we suppress the warning.
    // NOLINTNEXTLINE(misc-unconventional-assign-operator)
    SNMALLOC_FAST_PATH CapPtr<T, bounds> operator=(CapPtr<T, bounds> p) noexcept
    {
      this->store(p);
      return p;
    }

    SNMALLOC_FAST_PATH CapPtr<T, bounds>
    load(std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return CapPtr<T, bounds>(
        reinterpret_cast<T*>(this->unsafe_capptr.load(order)));
    }

    SNMALLOC_FAST_PATH void store(
      CapPtr<T, bounds> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      this->unsafe_capptr.store(desired.unsafe_capptr, order);
    }

    SNMALLOC_FAST_PATH CapPtr<T, bounds> exchange(
      CapPtr<T, bounds> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return CapPtr<T, bounds>(reinterpret_cast<T*>(
        this->unsafe_capptr.exchange(desired.unsafe_capptr, order)));
    }

    constexpr SNMALLOC_FAST_PATH bool operator==(const AtomicCapPtr& rhs) const
    {
      return this->unsafe_capptr == rhs.unsafe_capptr;
    }

    constexpr SNMALLOC_FAST_PATH bool operator!=(const AtomicCapPtr& rhs) const
    {
      return this->unsafe_capptr != rhs.unsafe_capptr;
    }

    constexpr SNMALLOC_FAST_PATH bool operator<(const AtomicCapPtr& rhs) const
    {
      return this->unsafe_capptr < rhs.unsafe_capptr;
    }
  };

  template<typename T>
  using AtomicCapPtrCBArena = AtomicCapPtr<T, CBArena>;

  template<typename T>
  using AtomicCapPtrCBChunk = AtomicCapPtr<T, CBChunk>;

  template<typename T>
  using AtomicCapPtrCBAlloc = AtomicCapPtr<T, CBAlloc>;

} // namespace snmalloc
