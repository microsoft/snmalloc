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
   */

  namespace capptr_bounds
  {
    /*
     * Bounds dimensions are sorted so that < reflects authority.  For example,
     * spatial::Alloc is less capable than spatial::Arena.
     */
    enum class spatial
    {
      Alloc,
      Chunk,
      ChunkD,
      Arena
    };
    enum class platform
    {
      Exported,
      High
    };

    template<spatial S, platform P>
    struct t
    {
      static constexpr enum spatial spatial = S;
      static constexpr enum platform platform = P;

      template<enum spatial SO>
      using with_spatial = t<SO, P>;

      template<enum platform PO>
      using with_platform = t<S, PO>;

      /*
       * The dimensions here are not used completely orthogonally.  In
       * particular, high-authority spatial bounds imply high-authority in other
       * dimensions and unchecked pointers must be annotated as tightly bounded.
       */
      static_assert(!(S == spatial::Arena) || (P == platform::High));
    };

    using CBArena = t<spatial::Arena, platform::High>;
    using CBChunk = t<spatial::Chunk, platform::High>;
    using CBChunkD = t<spatial::ChunkD, platform::High>;
    using CBChunkE = t<spatial::Chunk, platform::Exported>;
    using CBAlloc = t<spatial::Alloc, platform::High>;
    using CBAllocE = t<spatial::Alloc, platform::Exported>;

    // clang-format off
#ifdef __cpp_concepts
    template<typename T>
    concept c = requires()
    {
      { T::spatial } ->ConceptSame<const spatial>;
      { T::platform } ->ConceptSame<const platform>;
    };
#endif
    // clang-format on
  };

  /*
   * Defining these above and then `using` them to hide the namespace label like
   * this is still shorter than defining these completely out here, since there
   * isn't a notion of local namespace imports.
   */
  using CBArena = capptr_bounds::CBArena;
  using CBChunk = capptr_bounds::CBChunk;
  using CBChunkD = capptr_bounds::CBChunkD;
  using CBChunkE = capptr_bounds::CBChunkE;
  using CBAlloc = capptr_bounds::CBAlloc;
  using CBAllocE = capptr_bounds::CBAllocE;

  /**
   * Compute the "exported" variant of a capptr_bounds annotation.  This is
   * used by the PAL's capptr_export function to compute its return value's
   * annotation.
   */
  template<SNMALLOC_CONCEPT(capptr_bounds::c) B>
  using capptr_export_type =
    typename B::template with_platform<capptr_bounds::platform::Exported>;

  template<
    SNMALLOC_CONCEPT(capptr_bounds::c) BI,
    SNMALLOC_CONCEPT(capptr_bounds::c) BO>
  constexpr bool capptr_is_spatial_refinement()
  {
    if (BI::platform != BO::platform)
      return false;

    switch (BI::spatial)
    {
      using namespace capptr_bounds;
      case spatial::Arena:
        return true;

      case spatial::ChunkD:
        return BO::spatial == spatial::Alloc || BO::spatial == spatial::Chunk ||
          BO::spatial == spatial::ChunkD;

      case spatial::Chunk:
        return BO::spatial == spatial::Alloc || BO::spatial == spatial::Chunk ||
          BO::spatial == spatial::ChunkD;

      case spatial::Alloc:
        return BO::spatial == spatial::Alloc;
    }
  }

  /**
   * A pointer annotated with a "phantom type parameter" carrying a static
   * summary of its StrictProvenance metadata.
   */
  template<typename T, SNMALLOC_CONCEPT(capptr_bounds::c) bounds>
  struct CapPtr
  {
    T* unsafe_capptr;

    /**
     * nullptr is implicitly constructable at any bounds type
     */
    CapPtr(const std::nullptr_t n) : unsafe_capptr(n) {}

    CapPtr() : CapPtr(nullptr) {}

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
    explicit CapPtr(T* p) : unsafe_capptr(p) {}
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

    /**
     * Allow static_cast<>-s that preserve bounds but vary the target type.
     */
    template<typename U>
    SNMALLOC_FAST_PATH CapPtr<U, bounds> as_static()
    {
      return CapPtr<U, bounds>(static_cast<U*>(this->unsafe_capptr));
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

    SNMALLOC_FAST_PATH bool operator==(const CapPtr& rhs) const
    {
      return this->unsafe_capptr == rhs.unsafe_capptr;
    }

    SNMALLOC_FAST_PATH bool operator!=(const CapPtr& rhs) const
    {
      return this->unsafe_capptr != rhs.unsafe_capptr;
    }

    SNMALLOC_FAST_PATH bool operator<(const CapPtr& rhs) const
    {
      return this->unsafe_capptr < rhs.unsafe_capptr;
    }

    SNMALLOC_FAST_PATH T* operator->() const
    {
      /*
       * Alloc-bounded, platform-constrained pointers are associated with
       * objects coming from or going to the client; we should be doing nothing
       * with them.
       */
      static_assert(
        (bounds::spatial != capptr_bounds::spatial::Alloc) ||
        (bounds::platform != capptr_bounds::platform::Exported));
      return this->unsafe_capptr;
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
    return CapPtr<T, CBAlloc>(p.unsafe_capptr);
  }

  /**
   * With all the bounds and constraints in place, it's safe to extract a void
   * pointer (to reveal to the client).
   */
  SNMALLOC_FAST_PATH void* capptr_reveal(CapPtr<void, CBAllocE> p)
  {
    return p.unsafe_capptr;
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
  template<typename T, SNMALLOC_CONCEPT(capptr_bounds::c) bounds>
  struct AtomicCapPtr
  {
    std::atomic<T*> unsafe_capptr;

    /**
     * nullptr is constructable at any bounds type
     */
    AtomicCapPtr(const std::nullptr_t n) : unsafe_capptr(n) {}

    /**
     * Interconversion with CapPtr
     */
    AtomicCapPtr(CapPtr<T, bounds> p) : unsafe_capptr(p.unsafe_capptr) {}

    operator CapPtr<T, bounds>() const noexcept
    {
      return CapPtr<T, bounds>(this->unsafe_capptr);
    }

    // Our copy-assignment operator follows std::atomic and returns a copy of
    // the RHS.  Clang finds this surprising; we suppress the warning.
    // NOLINTNEXTLINE(misc-unconventional-assign-operator)
    CapPtr<T, bounds> operator=(CapPtr<T, bounds> p) noexcept
    {
      this->store(p);
      return p;
    }

    SNMALLOC_FAST_PATH CapPtr<T, bounds>
    load(std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return CapPtr<T, bounds>(this->unsafe_capptr.load(order));
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
      return CapPtr<T, bounds>(
        this->unsafe_capptr.exchange(desired.unsafe_capptr, order));
    }

    SNMALLOC_FAST_PATH bool operator==(const AtomicCapPtr& rhs) const
    {
      return this->unsafe_capptr == rhs.unsafe_capptr;
    }

    SNMALLOC_FAST_PATH bool operator!=(const AtomicCapPtr& rhs) const
    {
      return this->unsafe_capptr != rhs.unsafe_capptr;
    }

    SNMALLOC_FAST_PATH bool operator<(const AtomicCapPtr& rhs) const
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
