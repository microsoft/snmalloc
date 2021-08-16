#pragma once

#include "../ds/concept.h"
#include "../ds/defines.h"

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
   */

  namespace capptr
  {
    namespace dimension
    {
      /*
       * Describe the spatial extent (intended to be) authorized by a pointer.
       *
       * Bounds dimensions are sorted so that < reflects authority.
       */
      enum class Spatial
      {
        /**
         * Bounded to a particular allocation (which might be Large!)
         */
        Alloc,
        /**
         * Bounded to one or more particular chunk granules
         */
        Chunk,
      };

      /**
       * On some platforms (e.g., CHERI), pointers can be checked to see whether
       * they authorize control of the address space.  See the PAL's
       * capptr_export().
       */
      enum class AddressSpaceControl
      {
        /**
         * All intended control constraints have been applied.  For example, on
         * CheriBSD, the VMMAP permission has been stripped and so this CapPtr<>
         * cannot authorize manipulation of the address space itself, though it
         * continues to authorize loads and stores.
         */
        User,
        /**
         * No control constraints have been applied.  On CheriBSD, specifically,
         * this grants control of the address space (via mmap and friends) and
         * in Cornucopia exempts the pointer from revocation (as long as the
         * mapping remains in place, but snmalloc does not presently tear down
         * its own mappings.)
         */
        Full
      };

    } // namespace dimension

    /**
     * The aggregate type of a bound: a Cartesian product of the individual
     * dimensions, above.
     */
    template<dimension::Spatial S, dimension::AddressSpaceControl AS>
    struct bound
    {
      static constexpr enum dimension::Spatial spatial = S;
      static constexpr enum dimension::AddressSpaceControl
        address_space_control = AS;

      /**
       * Set just the spatial component of the bounds
       */
      template<enum dimension::Spatial SO>
      using with_spatial = bound<SO, AS>;

      /**
       * Set just the address space control component of the bounds
       */
      template<enum dimension::AddressSpaceControl ASO>
      using with_address_space_control = bound<S, ASO>;
    };

    // clang-format off
#ifdef __cpp_concepts
    /*
     * This is spelled a little differently from our other concepts because GCC
     * treats "{ T::spatial }" as generating a reference and then complains that
     * it isn't "ConceptSame<const Spatial>", though clang is perfectly happy
     * with that spelling.  Both seem happy with this formulation.
     */
    template<typename T>
    concept ConceptBound =
      ConceptSame<decltype(T::spatial), const capptr::dimension::Spatial> &&
      ConceptSame<decltype(T::address_space_control),
        const capptr::dimension::AddressSpaceControl>;
#endif
    // clang-format on

    /*
     * Several combinations are used often enough that we give convenient
     * aliases for them.
     */
    namespace bounds
    {
      /**
       * Internal access to a Chunk of memory.  These flow between the ASM and
       * the slab allocators, for example.
       */
      using Chunk =
        bound<dimension::Spatial::Chunk, dimension::AddressSpaceControl::Full>;

      /**
       * User access to an entire Chunk.  Used as an ephemeral state when
       * returning a large allocation.  See capptr_chunk_is_alloc.
       */
      using ChunkUser =
        Chunk::with_address_space_control<dimension::AddressSpaceControl::User>;

      /**
       * Internal access to just one allocation (usually, within a slab).
       */
      using AllocFull = Chunk::with_spatial<dimension::Spatial::Alloc>;

      /**
       * User access to just one allocation (usually, within a slab).
       */
      using Alloc = AllocFull::with_address_space_control<
        dimension::AddressSpaceControl::User>;
    } // namespace bounds
  } // namespace capptr

  /**
   * Determine whether BI is a spatial refinement of BO.
   * Chunk and ChunkD are considered eqivalent here.
   */
  template<
    SNMALLOC_CONCEPT(capptr::ConceptBound) BI,
    SNMALLOC_CONCEPT(capptr::ConceptBound) BO>
  SNMALLOC_CONSTEVAL bool capptr_is_spatial_refinement()
  {
    if (BI::address_space_control != BO::address_space_control)
    {
      return false;
    }

    switch (BI::spatial)
    {
      using namespace capptr::dimension;
      case Spatial::Chunk:
        return true;

      case Spatial::Alloc:
        return BO::spatial == Spatial::Alloc;
    }
  }

  /**
   * A pointer annotated with a "phantom type parameter" carrying a static
   * summary of its StrictProvenance metadata.
   */
  template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) bounds>
  struct CapPtr
  {
    T* unsafe_capptr;

    /**
     * nullptr is implicitly constructable at any bounds type
     */
    constexpr SNMALLOC_FAST_PATH CapPtr(const std::nullptr_t n)
    : unsafe_capptr(n)
    {}

    constexpr SNMALLOC_FAST_PATH CapPtr() : CapPtr(nullptr) {}

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
    constexpr explicit SNMALLOC_FAST_PATH CapPtr(T* p) : unsafe_capptr(p) {}
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

    /**
     * Allow static_cast<>-s that preserve bounds but vary the target type.
     */
    template<typename U>
    [[nodiscard]] SNMALLOC_FAST_PATH CapPtr<U, bounds> as_static() const
    {
      return CapPtr<U, bounds>(static_cast<U*>(this->unsafe_capptr));
    }

    [[nodiscard]] SNMALLOC_FAST_PATH CapPtr<void, bounds> as_void() const
    {
      return this->as_static<void>();
    }

    /**
     * A more aggressive bounds-preserving cast, using reinterpret_cast
     */
    template<typename U>
    [[nodiscard]] SNMALLOC_FAST_PATH CapPtr<U, bounds> as_reinterpret() const
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
      return this->unsafe_capptr;
    }

    [[nodiscard]] SNMALLOC_FAST_PATH T* unsafe_ptr() const
    {
      return this->unsafe_capptr;
    }

    [[nodiscard]] SNMALLOC_FAST_PATH uintptr_t unsafe_uintptr() const
    {
      return reinterpret_cast<uintptr_t>(this->unsafe_capptr);
    }
  };

  namespace capptr
  {
    /*
     * Aliases for CapPtr<> types with particular bounds.
     */

    template<typename T>
    using Chunk = CapPtr<T, bounds::Chunk>;

    template<typename T>
    using ChunkUser = CapPtr<T, bounds::ChunkUser>;

    template<typename T>
    using AllocFull = CapPtr<T, bounds::AllocFull>;

    template<typename T>
    using Alloc = CapPtr<T, bounds::Alloc>;

  } // namespace capptr

  static_assert(sizeof(capptr::Chunk<void>) == sizeof(void*));
  static_assert(alignof(capptr::Chunk<void>) == alignof(void*));

  /**
   * Sometimes (with large allocations) we really mean the entire chunk (or even
   * several chunks) to be the allocation.
   */
  template<typename T>
  inline SNMALLOC_FAST_PATH capptr::Alloc<T>
  capptr_chunk_is_alloc(capptr::ChunkUser<T> p)
  {
    return capptr::Alloc<T>(p.unsafe_capptr);
  }

  /**
   * With all the bounds and constraints in place, it's safe to extract a void
   * pointer (to reveal to the client).  Roughly dual to capptr_from_client().
   */
  inline SNMALLOC_FAST_PATH void* capptr_reveal(capptr::Alloc<void> p)
  {
    return p.unsafe_capptr;
  }

  /**
   * Given a void* from the client, it's fine to call it Alloc.  Roughly
   * dual to capptr_reveal().
   */
  static inline SNMALLOC_FAST_PATH capptr::Alloc<void>
  capptr_from_client(void* p)
  {
    return capptr::Alloc<void>(p);
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
  template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) bounds>
  struct AtomicCapPtr
  {
    std::atomic<T*> unsafe_capptr;

    /**
     * nullptr is constructable at any bounds type
     */
    constexpr SNMALLOC_FAST_PATH AtomicCapPtr(const std::nullptr_t n)
    : unsafe_capptr(n)
    {}

    /**
     * Interconversion with CapPtr
     */
    constexpr SNMALLOC_FAST_PATH AtomicCapPtr(CapPtr<T, bounds> p)
    : unsafe_capptr(p.unsafe_capptr)
    {}

    operator CapPtr<T, bounds>() const noexcept
    {
      return CapPtr<T, bounds>(this->unsafe_capptr);
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

  namespace capptr
  {
    /*
     * Aliases for AtomicCapPtr<> types with particular bounds.
     */

    template<typename T>
    using AtomicChunk = AtomicCapPtr<T, bounds::Chunk>;

    template<typename T>
    using AtomicChunkUser = AtomicCapPtr<T, bounds::ChunkUser>;

    template<typename T>
    using AtomicAllocFull = AtomicCapPtr<T, bounds::AllocFull>;

    template<typename T>
    using AtomicAlloc = AtomicCapPtr<T, bounds::Alloc>;

  } // namespace capptr

} // namespace snmalloc
