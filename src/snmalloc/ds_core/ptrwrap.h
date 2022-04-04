#pragma once

#include "concept.h"
#include "defines.h"

#include <atomic>

namespace snmalloc
{
  /*
   * reinterpret_cast<> is a powerful primitive that, excitingly, does not
   * require the programmer to annotate the expected *source* type.  We
   * therefore wrap its use to interconvert between uintptr_t and pointer types.
   */

  /**
   * Convert a pointer to a uintptr_t.  Template argument inference is
   * prohibited.
   */
  template<typename T>
  SNMALLOC_FAST_PATH_INLINE uintptr_t
  unsafe_to_uintptr(std::enable_if_t<true, T>* p)
  {
    return reinterpret_cast<uintptr_t>(p);
  }

  /** Convert a uintptr_t to a T*. */
  template<typename T>
  SNMALLOC_FAST_PATH_INLINE T* unsafe_from_uintptr(uintptr_t p)
  {
    return reinterpret_cast<T*>(p);
  }

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
       * capptr_to_user_address_control().
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

      /**
       * Distinguish pointers proximate provenance: pointers given to us by
       * clients can be arbitrarily malformed while pointers from the kernel or
       * internally can be presumed well-formed.  See the Backend's
       * capptr_domesticate().
       */
      enum class Wildness
      {
        /**
         * The purported "pointer" here may just be a pile of bits.  On CHERI
         * architectures, for example, it may not have a set tag or may be out
         * of bounds.
         */
        Wild,
        /**
         * Either this pointer has provenance from the kernel or it has been
         * checked by capptr_dewild.
         */
        Tame
      };
    } // namespace dimension

    /**
     * The aggregate type of a bound: a Cartesian product of the individual
     * dimensions, above.
     */
    template<
      dimension::Spatial S,
      dimension::AddressSpaceControl AS,
      dimension::Wildness W>
    struct bound
    {
      static constexpr enum dimension::Spatial spatial = S;
      static constexpr enum dimension::AddressSpaceControl
        address_space_control = AS;
      static constexpr enum dimension::Wildness wildness = W;

      /**
       * Set just the spatial component of the bounds
       */
      template<enum dimension::Spatial SO>
      using with_spatial = bound<SO, AS, W>;

      /**
       * Set just the address space control component of the bounds
       */
      template<enum dimension::AddressSpaceControl ASO>
      using with_address_space_control = bound<S, ASO, W>;

      /**
       * Set just the wild component of the bounds
       */
      template<enum dimension::Wildness WO>
      using with_wildness = bound<S, AS, WO>;

      /* The dimensions here are not used completely orthogonally */
      static_assert(
        !(W == dimension::Wildness::Wild) ||
          (S == dimension::Spatial::Alloc &&
           AS == dimension::AddressSpaceControl::User),
        "Wild pointers must be annotated as tightly bounded");
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
      ConceptSame<decltype(T::spatial), const dimension::Spatial> &&
      ConceptSame<decltype(T::address_space_control),
        const dimension::AddressSpaceControl> &&
      ConceptSame<decltype(T::wildness), const dimension::Wildness>;
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
      using Chunk = bound<
        dimension::Spatial::Chunk,
        dimension::AddressSpaceControl::Full,
        dimension::Wildness::Tame>;

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

      /**
       * A wild (i.e., putative) CBAllocExport pointer handed back by the
       * client. See capptr_from_client() and capptr_domesticate().
       */
      using AllocWild = Alloc::with_wildness<dimension::Wildness::Wild>;
    } // namespace bounds

    /**
     * Compute the AddressSpaceControl::User variant of a capptr::bound
     * annotation.  This is used by the PAL's capptr_to_user_address_control
     * function to compute its return value's annotation.
     */
    template<SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    using user_address_control_type =
      typename B::template with_address_space_control<
        dimension::AddressSpaceControl::User>;

    /**
     * Determine whether BI is a spatial refinement of BO.
     * Chunk and ChunkD are considered eqivalent here.
     */
    template<
      SNMALLOC_CONCEPT(capptr::ConceptBound) BI,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BO>
    SNMALLOC_CONSTEVAL bool is_spatial_refinement()
    {
      if (BI::address_space_control != BO::address_space_control)
      {
        return false;
      }

      if (BI::wildness != BO::wildness)
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
  } // namespace capptr

  /**
   * A pointer annotated with a "phantom type parameter" carrying a static
   * summary of its StrictProvenance metadata.
   */
  template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) bounds>
  class CapPtr
  {
    T* unsafe_capptr;

  public:
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
      static_assert(
        bounds::wildness != capptr::dimension::Wildness::Wild,
        "Trying to dereference a Wild pointer");
      return this->unsafe_capptr;
    }

    [[nodiscard]] SNMALLOC_FAST_PATH T* unsafe_ptr() const
    {
      return this->unsafe_capptr;
    }

    [[nodiscard]] SNMALLOC_FAST_PATH uintptr_t unsafe_uintptr() const
    {
      return unsafe_to_uintptr<T>(this->unsafe_capptr);
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

    template<typename T>
    using AllocWild = CapPtr<T, bounds::AllocWild>;

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
    return capptr::Alloc<T>(p.unsafe_ptr());
  }

  /**
   * With all the bounds and constraints in place, it's safe to extract a void
   * pointer (to reveal to the client).  Roughly dual to capptr_from_client(),
   * but we stop oursevles from revealing anything not known to be domesticated.
   */
  inline SNMALLOC_FAST_PATH void* capptr_reveal(capptr::Alloc<void> p)
  {
    return p.unsafe_ptr();
  }

  /**
   * Like capptr_reveal, but sometimes we do mean to reveal wild pointers
   * (specifically in external_pointer, where we're revealing something
   * architecturally derived from a user pointer).
   */
  inline SNMALLOC_FAST_PATH void* capptr_reveal_wild(capptr::AllocWild<void> p)
  {
    return p.unsafe_ptr();
  }

  /**
   * Given a void* from the client, it's fine to call it AllocWild.
   * Roughly dual to capptr_reveal().
   */
  static inline SNMALLOC_FAST_PATH capptr::AllocWild<void>
  capptr_from_client(void* p)
  {
    return capptr::AllocWild<void>(p);
  }

  /**
   * It's safe to mark any CapPtr as Wild.
   */
  template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) B>
  static inline SNMALLOC_FAST_PATH CapPtr<
    T,
    typename B::template with_wildness<capptr::dimension::Wildness::Wild>>
  capptr_rewild(CapPtr<T, B> p)
  {
    return CapPtr<
      T,
      typename B::template with_wildness<capptr::dimension::Wildness::Wild>>(
      p.unsafe_ptr());
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
  class AtomicCapPtr
  {
    std::atomic<T*> unsafe_capptr;

  public:
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
      this->unsafe_capptr.store(desired.unsafe_ptr(), order);
    }

    SNMALLOC_FAST_PATH CapPtr<T, bounds> exchange(
      CapPtr<T, bounds> desired,
      std::memory_order order = std::memory_order_seq_cst) noexcept
    {
      return CapPtr<T, bounds>(
        this->unsafe_capptr.exchange(desired.unsafe_ptr(), order));
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
