#pragma once

#ifdef _MSC_VER
#  define ALLOCATOR __declspec(allocator)
#else
#  define ALLOCATOR
#endif

#include "../test/histogram.h"
#include "allocstats.h"
#include "largealloc.h"
#include "mediumslab.h"
#include "pagemap.h"
#include "pooled.h"
#include "remoteallocator.h"
#include "sizeclasstable.h"
#include "slab.h"

namespace snmalloc
{
  enum Boundary
  {
    /**
     * The location of the first byte of this allocation.
     */
    Start,
    /**
     * The location of the last byte of the allocation.
     */
    End,
    /**
     * The location one past the end of the allocation.  This is mostly useful
     * for bounds checking, where anything less than this value is safe.
     */
    OnePastEnd
  };

  enum PageMapSuperslabKind
  {
    PMNotOurs = 0,
    PMSuperslab = 1,
    PMMediumslab = 2

    /*
     * Values 3 (inclusive) through SUPERSLAB_BITS (exclusive) are as yet
     * unused.
     *
     * Values SUPERSLAB_BITS (inclusive) through 64 (exclusive, as it would
     * represent the entire address space) are used for log2(size) at the
     * heads of large allocations.  See SuperslabMap::set_large_size.
     *
     * Values 64 (inclusive) through 128 (exclusive) are used for entries
     * within a large allocation.  A value of x at pagemap entry p indicates
     * that there are at least 2^(x-64) (inclusive) and at most 2^(x+1-64)
     * (exclusive) page map entries between p and the start of the
     * allocation.  See SuperslabMap::set_large_size and external_address's
     * handling of large reallocation redirections.
     *
     * Values 128 (inclusive) through 255 (inclusive) are as yet unused.
     */

  };

  /* Ensure that PageMapSuperslabKind values are actually disjoint */
  static_assert(SUPERSLAB_BITS > 2, "Large allocations possibly too small");

  /*
   * In CHERI, we have to be able to rederive pointers to headers and
   * metadata given the address of the allocation, since the capabilities we
   * give out have bounds narrowed to the allocation itself.  Since snmalloc
   * already holds a map of the address space, here's a great place to do
   * that.  Rather than store sizes per each SUPERSLAB_SIZE sized piece of
   * memory, we store a capability.
   *
   * We have a lot of "metadata" bits at the least-significant end of the
   * address of a capability in this map, since its bounds must, at least,
   * cover a SUPERSLAB_SIZE sized object (or a large allocation).  To
   * minimize churn, we stash the existing enum PageMapSuperslabKind values
   * in the bottom 8 bits of the address.
   *
   * We could cut that down to 6 bits by reclaiming all values above 64; we
   * can test that the capability given to us to free is has address equal
   * to the base of the capability stored here in the page map.
   */

  /**
   * PMNotOurs expressed as a pointer, for use with
   * SNMALLOC_PAGEMAP_POINTERS.
   */
  static constexpr void* PMNotOursPtr = nullptr;

  /**
   * Minimum alignment required for SNMALLOC_PAGEMAP_POINTERS so that we can
   * continue to store metadata in the bottom bits.
   */
  static constexpr int PAGEMAP_PTR_ALIGN = 0x100;

  /**
   * The type of things being stored in the pagemap structure.
   */
  using PagemapValueType =
    std::conditional_t<SNMALLOC_PAGEMAP_POINTERS, void*, uint8_t>;

  /**
   * The zero value of PagemapValueType.
   *
   * This should be equal to PMNotOurs or PMNotOursPtr, depending on
   * SNMALLOC_PAGEMAP_POINTERS.
   */
  static constexpr PagemapValueType PAGEMAP_VALUE_ZERO = 0;

#ifndef SNMALLOC_MAX_FLATPAGEMAP_SIZE
// Use flat map is under a single node.
#  define SNMALLOC_MAX_FLATPAGEMAP_SIZE PAGEMAP_NODE_SIZE
#endif
  static constexpr bool USE_FLATPAGEMAP =
    (pal_supports<LazyCommit>() &&
     ((1 << 24) >= sizeof(FlatPagemap<SUPERSLAB_BITS, PagemapValueType>))) ||
    (SNMALLOC_MAX_FLATPAGEMAP_SIZE >=
     sizeof(FlatPagemap<SUPERSLAB_BITS, PagemapValueType>));

  using SuperslabPagemap = std::conditional_t<
    USE_FLATPAGEMAP,
    FlatPagemap<SUPERSLAB_BITS, PagemapValueType>,
    Pagemap<SUPERSLAB_BITS, PagemapValueType, PAGEMAP_VALUE_ZERO>>;

  /**
   * Mixin used by `SuperslabMap` to directly access the pagemap via a global
   * variable.  This should be used from within the library or program that
   * owns the pagemap.
   *
   * This class makes the global pagemap a static field so that its name
   * includes the type mangling.  If two compilation units try to instantiate
   * two different types of pagemap then they will see two distinct pagemaps.
   * This will prevent allocating with one and freeing with the other (because
   * the memory will show up as not owned by any allocator in the other
   * compilation unit) but will prevent the same memory being interpreted as
   * having two different types.
   */
  template<typename T>
  class GlobalPagemapTemplate
  {
    /**
     * The global pagemap variable.  The name of this symbol will include the
     * type of `T`.
     */
    inline static T global_pagemap;

  public:
    /**
     * Returns the pagemap.
     */
    static SuperslabPagemap& pagemap()
    {
      return global_pagemap;
    }
  };

  using GlobalPagemap = GlobalPagemapTemplate<SuperslabPagemap>;

  /**
   * Optionally exported function that accesses the global pagemap provided by
   * a shared library.
   */
  extern "C" void* snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);

  /**
   * Mixin used by `SuperslabMap` to access the global pagemap via a
   * type-checked C interface.  This should be used when another library (e.g.
   * your C standard library) uses snmalloc and you wish to use a different
   * configuration in your program or library, but wish to share a pagemap so
   * that either version can deallocate memory.
   */
  class ExternalGlobalPagemap
  {
    /**
     * A pointer to the pagemap.
     */
    inline static SuperslabPagemap* external_pagemap;

  public:
    /**
     * Constructor.  Accesses the pagemap via the C ABI accessor and casts it to
     * the expected type, failing in cases of ABI mismatch.
     */
    ExternalGlobalPagemap()
    {
      const snmalloc::PagemapConfig* c;
      external_pagemap =
        SuperslabPagemap::cast_to_pagemap(snmalloc_pagemap_global_get(&c), c);
      // FIXME: Report an error somehow in non-debug builds.
      assert(external_pagemap);
    }

    /**
     * Returns the exported pagemap.
     */
    static SuperslabPagemap& pagemap()
    {
      return *external_pagemap;
    }
  };

  /**
   * Class that defines an interface to the pagemap.  This is provided to
   * `Allocator` as a template argument and so can be replaced by a compatible
   * implementation (for example, to move pagemap updates to a different
   * protection domain).
   */
  template<typename PagemapProvider = GlobalPagemap>
  struct SuperslabMap : public PagemapProvider
  {
    using PagemapProvider::PagemapProvider;
    /**
     * Get the pagemap entry corresponding to a specific address.
     *
     * Despite the type, the return value is an enum PageMapSuperslabKind
     * or one of the reserved values described therewith.
     */
    uint8_t get(address_t p)
    {
      if constexpr (SNMALLOC_PAGEMAP_POINTERS)
      {
        return static_cast<uintptr_t>(PagemapProvider::pagemap().get(p));
      }
      else
      {
        return PagemapProvider::pagemap().get(p);
      }
    }

    /**
     * Get the pagemap entry corresponding to a specific address.
     */
    uint8_t get(void* p)
    {
      return get(address_cast(p));
    }

    /**
     * Fetch a pointer through the pagemap.  The return value and the
     * argument point at the same location, if the template parameter offset
     * is true, but may have different metadata associated with them, such
     * as CHERI bounds.  If the pagemap is storing pointers, then the
     * template parameter offset may be false, in which case, the return
     * value is exactly the pagemap entry, which will point at the bottom of
     * the pagemap granule and include the argument in its bounds.
     */
    template<bool offset = true>
    void* getp(void* p)
    {
      if constexpr (SNMALLOC_PAGEMAP_POINTERS)
      {
        void* pmp = pointer_align_down<PAGEMAP_PTR_ALIGN, void>(
          PagemapProvider::pagemap().get(address_cast(p)));
        if constexpr (offset)
        {
          return pointer_offset(pmp, pointer_diff(pmp, p));
        }
        else
        {
          return pmp;
        }
      }
      else
      {
        static_assert(offset);
        return p;
      }
    }

    /**
     * Set a pagemap entry indicating that there is a superslab at the
     * specified index.
     */
    void set_slab(Superslab* slab)
    {
      set(slab, PMSuperslab);
    }
    /**
     * Add a pagemap entry indicating that a medium slab has been allocated.
     */
    void set_slab(Mediumslab* slab)
    {
      set(slab, PMMediumslab);
    }
    /**
     * Remove an entry from the pagemap corresponding to a superslab.
     */
    void clear_slab(Superslab* slab)
    {
      assert(get(slab) == PMSuperslab);
      clear(slab);
    }
    /**
     * Remove an entry corresponding to a medium slab.
     */
    void clear_slab(Mediumslab* slab)
    {
      assert(get(slab) == PMMediumslab);
      clear(slab);
    }
    /**
     * Update the pagemap to reflect a large allocation, of `size` bytes from
     * address `p`.
     */
    void set_large_size(void* p, size_t size)
    {
      size_t size_bits = bits::next_pow2_bits(size);
      if constexpr (SNMALLOC_PAGEMAP_POINTERS)
      {
        set(p, pointer_offset(p, size_bits));
        // Set redirect slide
        char* ss = reinterpret_cast<char*>(pointer_offset(p, SUPERSLAB_SIZE));
        for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
        {
          size_t run = 1ULL << i;
          PagemapProvider::pagemap().set_range(
            address_cast(ss), pointer_offset(p, 64 + i + SUPERSLAB_BITS), run);
          ss += SUPERSLAB_SIZE * run;
        }
      }
      else
      {
        set(p, static_cast<uint8_t>(size_bits));
        // Set redirect slide
        auto ss = address_cast(p) + SUPERSLAB_SIZE;
        for (size_t i = 0; i < size_bits - SUPERSLAB_BITS; i++)
        {
          size_t run = 1ULL << i;
          PagemapProvider::pagemap().set_range(
            ss, static_cast<uint8_t>(64 + i + SUPERSLAB_BITS), run);
          ss = ss + SUPERSLAB_SIZE * run;
        }
      }
    }
    /**
     * Update the pagemap to remove a large allocation, of `size` bytes from
     * address `p`.
     */
    void clear_large_size(void* vp, size_t size)
    {
      auto p = address_cast(vp);
      size_t rounded_size = bits::next_pow2(size);
      assert(get(p) == bits::next_pow2_bits(size));
      auto count = rounded_size >> SUPERSLAB_BITS;
      if constexpr (SNMALLOC_PAGEMAP_POINTERS)
      {
        PagemapProvider::pagemap().set_range(p, PMNotOursPtr, count);
      }
      else
      {
        PagemapProvider::pagemap().set_range(p, PMNotOurs, count);
      }
    }

  private:
    /**
     * Helper function to set a pagemap entry.  This is not part of the public
     * interface and exists to make it easy to reuse the code in the public
     * methods in other pagemap adaptors.
     */
    void set(void* p, uint8_t x)
    {
      if constexpr (SNMALLOC_PAGEMAP_POINTERS) {
        PagemapProvider::pagemap().set(address_cast(p), pointer_offset(p, x));
      } else {
        PagemapProvider::pagemap().set(address_cast(p), x);
      }
    }

    /**
     * Helper function to clear a pagemap entry.
     */
    void clear(void *p)
    {
      if constexpr (SNMALLOC_PAGEMAP_POINTERS) {
        PagemapProvider::pagemap().set(address_cast(p), PMNotOursPtr);
      } else {
        PagemapProvider::pagemap().set(address_cast(p), PMNotOurs);
      }
    }
  };

#ifndef SNMALLOC_DEFAULT_PAGEMAP
#  define SNMALLOC_DEFAULT_PAGEMAP snmalloc::SuperslabMap<>
#endif

  // This class is just used so that the free lists are the first entry
  // in the allocator and hence has better code gen.
  // It contains a free list per small size class.  These are used for
  // allocation on the fast path. This part of the code is inspired by mimalloc.
  class FastFreeLists
  {
  protected:
    FreeListHead small_fast_free_lists[NUM_SMALL_CLASSES];

  public:
    FastFreeLists() : small_fast_free_lists() {}
  };

  SNMALLOC_FAST_PATH void* no_replacement(void*)
  {
    return nullptr;
  }

  /**
   * Allocator.  This class is parameterised on three template parameters.  The
   * `MemoryProvider` defines the source of memory for this allocator.
   * Allocators try to reuse address space by allocating from existing slabs or
   * reusing freed large allocations.  When they need to allocate a new chunk
   * of memory they request space from the `MemoryProvider`.
   *
   * The `PageMap` parameter provides the adaptor to the pagemap.  This is used
   * to associate metadata with large (16MiB, by default) regions, allowing an
   * allocator to find the allocator responsible for that region.
   *
   * The next template parameter, `IsQueueInline`, defines whether the
   * message queue for this allocator should be stored as a field of the
   * allocator (`true`) or provided externally, allowing it to be anywhere else
   * in the address space (`false`).
   *
   * The final template parameter provides a hook to allow the allocator in use
   * to be dynamically modified.  This is used to implement a trick from
   * mimalloc that avoids a conditional branch on the fast path.  We initialise
   * the thread-local allocator pointer with the address of a global allocator,
   * which never owns any memory.  When we try to allocate memory, we call the
   * replacement function.
   */
  template<
    class MemoryProvider = GlobalVirtual,
    class PageMap = SNMALLOC_DEFAULT_PAGEMAP,
    bool IsQueueInline = true,
    void* (*Replacement)(void*) = no_replacement>
  class Allocator
  : public FastFreeLists,
    public Pooled<
      Allocator<MemoryProvider, PageMap, IsQueueInline, Replacement>>
  {
    LargeAlloc<MemoryProvider> large_allocator;
    PageMap page_map;

  public:
    Stats& stats()
    {
      return large_allocator.stats;
    }

    template<class MP>
    friend class AllocPool;

    /**
     * Allocate memory of a statically known size.
     */
    template<
      size_t size,
      ZeroMem zero_mem = NoZero,
      AllowReserve allow_reserve = YesReserve>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc()
    {
      static_assert(size != 0, "Size must not be zero.");
#ifdef USE_MALLOC
      static_assert(
        allow_reserve == YesReserve,
        "When passing to malloc, cannot require NoResereve");
      if constexpr (zero_mem == NoZero)
        return malloc(size);
      else
        return calloc(1, size);
#else
      constexpr sizeclass_t sizeclass = size_to_sizeclass_const(size);

      stats().alloc_request(size);

      if constexpr (sizeclass < NUM_SMALL_CLASSES)
      {
        return small_alloc<zero_mem, allow_reserve>(size);
      }
      else if constexpr (sizeclass < NUM_SIZECLASSES)
      {
        handle_message_queue();
        constexpr size_t rsize = sizeclass_to_size(sizeclass);
        return medium_alloc<zero_mem, allow_reserve>(sizeclass, rsize, size);
      }
      else
      {
        handle_message_queue();
        return large_alloc<zero_mem, allow_reserve>(size);
      }
#endif
    }

    /**
     * Allocate memory of a dynamically known size.
     */
    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc(size_t size)
    {
#ifdef USE_MALLOC
      static_assert(
        allow_reserve == YesReserve,
        "When passing to malloc, cannot require NoResereve");
      if constexpr (zero_mem == NoZero)
        return malloc(size);
      else
        return calloc(1, size);
#else
      stats().alloc_request(size);

      // Perform the - 1 on size, so that zero wraps around and ends up on
      // slow path.
      if (likely((size - 1) <= (sizeclass_to_size(NUM_SMALL_CLASSES - 1) - 1)))
      {
        // Allocations smaller than the slab size are more likely. Improve
        // branch prediction by placing this case first.
        return small_alloc<zero_mem, allow_reserve>(size);
      }

      return alloc_not_small<zero_mem, allow_reserve>(size);
    }

    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    SNMALLOC_SLOW_PATH ALLOCATOR void* alloc_not_small(size_t size)
    {
      handle_message_queue();

      if (size == 0)
      {
        return small_alloc<zero_mem, allow_reserve>(1);
      }

      sizeclass_t sizeclass = size_to_sizeclass(size);
      if (sizeclass < NUM_SIZECLASSES)
      {
        size_t rsize = sizeclass_to_size(sizeclass);
        return medium_alloc<zero_mem, allow_reserve>(sizeclass, rsize, size);
      }

      return large_alloc<zero_mem, allow_reserve>(size);

#endif
    }

    /*
     * Free memory of a statically known size. Must be called with an
     * external pointer.
     */
    template<size_t size>
    void dealloc(void* p)
    {
#ifdef USE_MALLOC
      UNUSED(size);
      return free(p);
#else

      constexpr sizeclass_t sizeclass = size_to_sizeclass_const(size);

      handle_message_queue();

      p = pagemap().getp(p);

      if (sizeclass < NUM_SMALL_CLASSES)
      {
        Superslab* super = Superslab::get(p);
        RemoteAllocator* target = super->get_allocator();

        if (target == public_state())
          small_dealloc(super, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
      }
      else if (sizeclass < NUM_SIZECLASSES)
      {
        Mediumslab* slab = Mediumslab::get(p);
        RemoteAllocator* target = slab->get_allocator();

        if (target == public_state())
          medium_dealloc(slab, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
      }
      else
      {
        large_dealloc(p, size);
      }
#endif
    }

    /*
     * Free memory of a dynamically known size. Must be called with an
     * external pointer.
     */
    void dealloc(void* p, size_t size)
    {
#ifdef USE_MALLOC
      UNUSED(size);
      return free(p);
#else
      handle_message_queue();

      p = pagemap().getp(p);

      sizeclass_t sizeclass = size_to_sizeclass(size);

      if (sizeclass < NUM_SMALL_CLASSES)
      {
        Superslab* super = Superslab::get(p);
        RemoteAllocator* target = super->get_allocator();

        if (target == public_state())
          small_dealloc(super, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
      }
      else if (sizeclass < NUM_SIZECLASSES)
      {
        Mediumslab* slab = Mediumslab::get(p);
        RemoteAllocator* target = slab->get_allocator();

        if (target == public_state())
          medium_dealloc(slab, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
      }
      else
      {
        large_dealloc(p, size);
      }
#endif
    }

    /*
     * Free memory of an unknown size. Must be called with an external
     * pointer.
     */
    SNMALLOC_FAST_PATH void dealloc(void* p)
    {
#ifdef USE_MALLOC
      return free(p);
#else

      uint8_t size = pagemap().get(address_cast(p));

      p = pagemap().getp(p);

      Superslab* super = Superslab::get(p);

      if (likely(size == PMSuperslab))
      {
        RemoteAllocator* target = super->get_allocator();
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have not yet deallocated this
        // pointer.
        sizeclass_t sizeclass = meta.sizeclass;

        if (likely(super->get_allocator() == public_state()))
          small_dealloc(super, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
        return;
      }
      dealloc_not_small(p, size);
    }

    SNMALLOC_SLOW_PATH void dealloc_not_small(void* p, uint8_t size)
    {
      handle_message_queue();

      if (p == nullptr)
        return;

      if (size == PMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p);
        RemoteAllocator* target = slab->get_allocator();

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have not yet deallocated this pointer.
        sizeclass_t sizeclass = slab->get_sizeclass();

        if (target == public_state())
          medium_dealloc(slab, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
        return;
      }

      if (size == 0)
      {
        error("Not allocated by this allocator");
      }

#  ifdef CHECK_CLIENT
      Superslab* super = Superslab::get(p);
      if (size > 64 || address_cast(super) != address_cast(p))
      {
        error("Not deallocating start of an object");
      }
#  endif
      large_dealloc(p, 1ULL << size);
#endif
    }

    template<Boundary location = Start>
    static address_t external_address(void* p)
    {
#ifdef USE_MALLOC
      error("Unsupported");
      UNUSED(p);
#else
      uint8_t size = PageMap::pagemap().get(address_cast(p));

      if constexpr (SNMALLOC_PAGEMAP_POINTERS)
      {
        p = pointer_align_down<PAGEMAP_PTR_ALIGN, void>(
          PageMap::pagemap().get(address_cast(p)));
      }

      Superslab* super = Superslab::get(p);
      if (size == PMSuperslab)
      {
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);

        sizeclass_t sc = meta.sizeclass;
        void* slab_end = pointer_offset(slab, SLAB_SIZE);

        return external_pointer<location>(p, sc, slab_end);
      }
      if (size == PMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p);

        sizeclass_t sc = slab->get_sizeclass();
        void* slab_end = pointer_offset(slab, SUPERSLAB_SIZE);

        return external_pointer<location>(p, sc, slab_end);
      }

      auto ss = address_cast(super);

      while (size > 64)
      {
        // This is a large alloc redirect.
        ss = ss - (1ULL << (size - 64));
        size = PageMap::pagemap().get(ss);
      }

      if (size == 0)
      {
        if constexpr ((location == End) || (location == OnePastEnd))
          // We don't know the End, so return MAX_PTR
          return UINTPTR_MAX;
        else
          // We don't know the Start, so return MIN_PTR
          return 0;
      }

      // This is a large alloc, mask off to the slab size.
      if constexpr (location == Start)
        return ss;
      else if constexpr (location == End)
        return (ss + (1ULL << size) - 1ULL);
      else
        return (ss + (1ULL << size));
#endif
    }

    template<Boundary location = Start>
    static void* external_pointer(void* p)
    {
      return pointer_cast<void>(external_address<location>(p));
    }

    static size_t alloc_size(void* p)
    {
      // This must be called on an external pointer.
      size_t size = PageMap::pagemap().get(address_cast(p));

      if constexpr (SNMALLOC_PAGEMAP_POINTERS)
      {
        p = pointer_align_down<PAGEMAP_PTR_ALIGN, void>(
          PageMap::pagemap().get(address_cast(p)));
      }

      if (size == 0)
      {
        error("Not allocated by this allocator");
      }
      else if (size == PMSuperslab)
      {
        Superslab* super = Superslab::get(p);

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);

        return sizeclass_to_size(meta.sizeclass);
      }
      else if (size == PMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p);
        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        return sizeclass_to_size(slab->get_sizeclass());
      }

      return 1ULL << size;
    }

    size_t get_id()
    {
      return id();
    }

  private:
    using alloc_id_t = typename Remote::alloc_id_t;

    /*
     * A singly-linked list of Remote objects, supporting append and
     * take-all operations.  Intended only for the private use of this
     * allocator; the Remote objects here will later be taken and pushed
     * to the inter-thread message queues.
     */
    struct RemoteList
    {
      /*
       * A stub Remote object that will always be the head of this list;
       * never taken for further processing.
       */
      Remote head;

      Remote* last;

      RemoteList()
      {
        clear();
      }

      void clear()
      {
        last = &head;
      }

      bool empty()
      {
        return last == &head;
      }
    };

    struct RemoteCache
    {
      /**
       * The total amount of memory stored awaiting dispatch to other
       * allocators.  This is initialised to the maximum size that we use
       * before caching so that, when we hit the slow path and need to dispatch
       * everything, we can check if we are a real allocator and lazily provide
       * a real allocator.
       */
      size_t size = REMOTE_CACHE;
      RemoteList list[REMOTE_SLOTS];

      /// Used to find the index into the array of queues for remote
      /// deallocation
      /// r is used for which round of sending this is.
      inline size_t get_slot(size_t id, size_t r)
      {
        constexpr size_t allocator_size = sizeof(
          Allocator<MemoryProvider, PageMap, IsQueueInline, Replacement>);
        constexpr size_t initial_shift =
          bits::next_pow2_bits_const(allocator_size);
        assert((initial_shift + (r * REMOTE_SLOT_BITS)) < 64);
        return (id >> (initial_shift + (r * REMOTE_SLOT_BITS))) & REMOTE_MASK;
      }

      SNMALLOC_FAST_PATH void
      dealloc_sized(alloc_id_t target_id, void* p, size_t objectsize)
      {
        this->size += objectsize;

        Remote* r = static_cast<Remote*>(p);
        r->set_target_id(target_id);
        assert(r->target_id() == target_id);

        RemoteList* l = &list[get_slot(target_id, 0)];
        l->last->non_atomic_next = r;
        l->last = r;
      }

      SNMALLOC_FAST_PATH void
      dealloc(alloc_id_t target_id, void* p, sizeclass_t sizeclass)
      {
        dealloc_sized(target_id, p, sizeclass_to_size(sizeclass));
      }

      void post(alloc_id_t id)
      {
        // When the cache gets big, post lists to their target allocators.
        size = 0;

        size_t post_round = 0;

        while (true)
        {
          auto my_slot = get_slot(id, post_round);

          for (size_t i = 0; i < REMOTE_SLOTS; i++)
          {
            if (i == my_slot)
              continue;

            RemoteList* l = &list[i];
            Remote* first = l->head.non_atomic_next;

            if (!l->empty())
            {
              // Send all slots to the target at the head of the list.
              Superslab* super = Superslab::get(first);
              super->get_allocator()->message_queue.enqueue(first, l->last);
              l->clear();
            }
          }

          RemoteList* resend = &list[my_slot];
          if (resend->empty())
            break;

          // Entries could map back onto the "resend" list,
          // so take copy of the head, mark the last element,
          // and clear the original list.
          Remote* r = resend->head.non_atomic_next;
          resend->last->non_atomic_next = nullptr;
          resend->clear();

          post_round++;

          while (r != nullptr)
          {
            // Use the next N bits to spread out remote deallocs in our own
            // slot.
            size_t slot = get_slot(r->target_id(), post_round);
            RemoteList* l = &list[slot];
            l->last->non_atomic_next = r;
            l->last = r;

            r = r->non_atomic_next;
          }
        }
      }
    };

    SlabList small_classes[NUM_SMALL_CLASSES];
    DLList<Mediumslab> medium_classes[NUM_MEDIUM_CLASSES];

    DLList<Superslab> super_available;
    DLList<Superslab> super_only_short_available;

    RemoteCache remote;

    std::conditional_t<IsQueueInline, RemoteAllocator, RemoteAllocator*>
      remote_alloc;

#ifdef CACHE_FRIENDLY_OFFSET
    size_t remote_offset = 0;

    void* apply_cache_friendly_offset(void* p, sizeclass_t sizeclass)
    {
      size_t mask = sizeclass_to_cache_friendly_mask(sizeclass);

      size_t offset = remote_offset & mask;
      remote_offset += CACHE_FRIENDLY_OFFSET;

      return (void*)((uintptr_t)p + offset);
    }
#else
    void* apply_cache_friendly_offset(void* p, sizeclass_t sizeclass)
    {
      UNUSED(sizeclass);
      return p;
    }
#endif

    auto* public_state()
    {
      if constexpr (IsQueueInline)
      {
        return &remote_alloc;
      }
      else
      {
        return remote_alloc;
      }
    }

    alloc_id_t id()
    {
      return public_state()->id();
    }

    auto& message_queue()
    {
      return public_state()->message_queue;
    }

    template<class A, class MemProvider>
    friend class Pool;

  public:
    Allocator(
      MemoryProvider& m,
      PageMap&& p = PageMap(),
      RemoteAllocator* r = nullptr,
      bool isFake = false)
    : large_allocator(m), page_map(p)
    {
      if constexpr (IsQueueInline)
      {
        assert(r == nullptr);
        (void)r;
      }
      else
      {
        remote_alloc = r;
      }

      if (id() >= static_cast<alloc_id_t>(-1))
        error("Id should not be -1");

      // If this is fake, don't do any of the bits of initialisation that may
      // allocate memory.
      if (isFake)
        return;

      init_message_queue();
      message_queue().invariant();

#ifndef NDEBUG
      for (sizeclass_t i = 0; i < NUM_SIZECLASSES; i++)
      {
        size_t size = sizeclass_to_size(i);
        sizeclass_t sc1 = size_to_sizeclass(size);
        sizeclass_t sc2 = size_to_sizeclass_const(size);
        size_t size1 = sizeclass_to_size(sc1);
        size_t size2 = sizeclass_to_size(sc2);

        // All medium size classes are page aligned.
        if (i > NUM_SMALL_CLASSES)
        {
          assert(is_aligned_block<OS_PAGE_SIZE>(nullptr, size1));
        }

        assert(sc1 == i);
        assert(sc1 == sc2);
        assert(size1 == size);
        assert(size1 == size2);
      }
#endif
    }

    template<Boundary location>
    static uintptr_t
    external_pointer(void* p, sizeclass_t sizeclass, void* end_point)
    {
      size_t rsize = sizeclass_to_size(sizeclass);

      void* end_point_correction = location == End ?
        (static_cast<uint8_t*>(end_point) - 1) :
        (location == OnePastEnd ? end_point :
                                  (static_cast<uint8_t*>(end_point) - rsize));

      ptrdiff_t offset_from_end =
        (static_cast<uint8_t*>(end_point) - 1) - static_cast<uint8_t*>(p);

      size_t end_to_end =
        round_by_sizeclass(rsize, static_cast<size_t>(offset_from_end));

      return address_cast<uint8_t>(
        static_cast<uint8_t*>(end_point_correction) - end_to_end);
    }

    void init_message_queue()
    {
      // Manufacture an allocation to prime the queue
      // Using an actual allocation removes a conditional of a critical path.
      Remote* dummy = reinterpret_cast<Remote*>(alloc<YesZero>(MIN_ALLOC_SIZE));
      dummy->set_target_id(id());
      message_queue().init(dummy);
    }

    SNMALLOC_FAST_PATH void handle_dealloc_remote(Remote* p)
    {
      Superslab* super = Superslab::get(p);

#ifdef CHECK_CLIENT
      if (p->target_id() != super->get_allocator()->id())
        error("Detected memory corruption.  Potential use-after-free");
#endif
      if (likely(super->get_kind() == Super))
      {
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);
        if (likely(p->target_id() == id()))
        {
          small_dealloc_offseted(super, p, meta.sizeclass);
          return;
        }
      }
      handle_dealloc_remote_slow(p);
    }

    SNMALLOC_SLOW_PATH void handle_dealloc_remote_slow(Remote* p)
    {
      Superslab* super = Superslab::get(p);
      if (likely(super->get_kind() == Medium))
      {
        Mediumslab* slab = Mediumslab::get(p);
        if (p->target_id() == id())
        {
          sizeclass_t sizeclass = slab->get_sizeclass();
          void* start = remove_cache_friendly_offset(p, sizeclass);
          medium_dealloc(slab, start, sizeclass);
        }
        else
        {
          // Queue for remote dealloc elsewhere.
          remote.dealloc(p->target_id(), p, slab->get_sizeclass());
        }
      }
      else
      {
        assert(likely(p->target_id() != id()));
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);
        // Queue for remote dealloc elsewhere.
        remote.dealloc(p->target_id(), p, meta.sizeclass);
      }
    }

    SNMALLOC_SLOW_PATH void handle_message_queue_inner()
    {
      for (size_t i = 0; i < REMOTE_BATCH; i++)
      {
        auto r = message_queue().dequeue();

        if (unlikely(!r.second))
          break;

        handle_dealloc_remote(r.first);
      }

      // Our remote queues may be larger due to forwarding remote frees.
      if (likely(remote.size < REMOTE_CACHE))
        return;

      stats().remote_post();
      remote.post(id());
    }

    SNMALLOC_FAST_PATH void handle_message_queue()
    {
      // Inline the empty check, but not necessarily the full queue handling.
      if (likely(message_queue().is_empty()))
        return;

      handle_message_queue_inner();
    }

    template<AllowReserve allow_reserve>
    Superslab* get_superslab()
    {
      Superslab* super = super_available.get_head();

      if (super != nullptr)
        return super;

      super = reinterpret_cast<Superslab*>(
        large_allocator.template alloc<NoZero, allow_reserve>(
          0, SUPERSLAB_SIZE));

      if ((allow_reserve == NoReserve) && (super == nullptr))
        return super;

      super->init(public_state());
      pagemap().set_slab(super);
      super_available.insert(super);
      return super;
    }

    void reposition_superslab(Superslab* super)
    {
      switch (super->get_status())
      {
        case Superslab::Full:
        {
          // Remove from the list of superslabs that have available slabs.
          super_available.remove(super);
          break;
        }

        case Superslab::Available:
        {
          // Do nothing.
          break;
        }

        case Superslab::OnlyShortSlabAvailable:
        {
          // Move from the general list to the short slab only list.
          super_available.remove(super);
          super_only_short_available.insert(super);
          break;
        }

        case Superslab::Empty:
        {
          // Can't be empty since we just allocated.
          error("Unreachable");
          break;
        }
      }
    }

    template<AllowReserve allow_reserve>
    Slab* alloc_slab(sizeclass_t sizeclass)
    {
      stats().sizeclass_alloc_slab(sizeclass);
      if (Superslab::is_short_sizeclass(sizeclass))
      {
        // Pull a short slab from the list of superslabs that have only the
        // short slab available.
        Superslab* super = super_only_short_available.pop();

        if (super != nullptr)
        {
          Slab* slab =
            super->alloc_short_slab(sizeclass, large_allocator.memory_provider);
          assert(super->is_full());
          return slab;
        }

        super = get_superslab<allow_reserve>();

        if ((allow_reserve == NoReserve) && (super == nullptr))
          return nullptr;

        Slab* slab =
          super->alloc_short_slab(sizeclass, large_allocator.memory_provider);
        reposition_superslab(super);
        return slab;
      }

      Superslab* super = get_superslab<allow_reserve>();

      if ((allow_reserve == NoReserve) && (super == nullptr))
        return nullptr;

      Slab* slab =
        super->alloc_slab(sizeclass, large_allocator.memory_provider);
      reposition_superslab(super);
      return slab;
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_FAST_PATH void* small_alloc(size_t size)
    {
      MEASURE_TIME_MARKERS(
        small_alloc,
        4,
        16,
        MARKERS(
          zero_mem == YesZero ? "zeromem" : "nozeromem",
          allow_reserve == NoReserve ? "noreserve" : "reserve"));

      SNMALLOC_ASSUME(size <= SLAB_SIZE);
      sizeclass_t sizeclass = size_to_sizeclass(size);

      assert(sizeclass < NUM_SMALL_CLASSES);
      auto& fl = small_fast_free_lists[sizeclass];
      void* head = fl.value;
      if (likely(head != nullptr))
      {
        stats().sizeclass_alloc(sizeclass);
        // Read the next slot from the memory that's about to be allocated.
        fl.value = Metaslab::follow_next(head);

        void* p = remove_cache_friendly_offset(head, sizeclass);
        if constexpr (zero_mem == YesZero)
        {
          large_allocator.memory_provider.zero(p, size);
        }
        return p;
      }

      return small_alloc_slow<zero_mem, allow_reserve>(sizeclass);
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_SLOW_PATH void* small_alloc_slow(sizeclass_t sizeclass)
    {
      if (void* replacement = Replacement(this))
      {
        return reinterpret_cast<Allocator*>(replacement)
          ->template small_alloc_slow<zero_mem, allow_reserve>(sizeclass);
      }

      stats().sizeclass_alloc(sizeclass);

      handle_message_queue();
      size_t rsize = sizeclass_to_size(sizeclass);
      auto& sl = small_classes[sizeclass];

      Slab* slab;

      if (!sl.is_empty())
      {
        SlabLink* link = sl.get_head();
        slab = link->get_slab();
      }
      else
      {
        slab = alloc_slab<allow_reserve>(sizeclass);

        if ((allow_reserve == NoReserve) && (slab == nullptr))
          return nullptr;

        sl.insert_back(slab->get_link());
      }
      auto& ffl = small_fast_free_lists[sizeclass];
      return slab->alloc<zero_mem>(
        sl, ffl, rsize, large_allocator.memory_provider);
    }

    SNMALLOC_FAST_PATH void
    small_dealloc(Superslab* super, void* p, sizeclass_t sizeclass)
    {
#ifdef CHECK_CLIENT
      Slab* slab = Slab::get(p);
      if (!slab->is_start_of_object(super, p))
      {
        error("Not deallocating start of an object");
      }
#endif

      void* offseted = apply_cache_friendly_offset(p, sizeclass);
      small_dealloc_offseted(super, offseted, sizeclass);
    }

    SNMALLOC_FAST_PATH void
    small_dealloc_offseted(Superslab* super, void* p, sizeclass_t sizeclass)
    {
      MEASURE_TIME(small_dealloc, 4, 16);
      stats().sizeclass_dealloc(sizeclass);

      Slab* slab = Slab::get(p);
      if (likely(slab->dealloc_fast(super, p)))
        return;

      small_dealloc_offseted_slow(super, p, sizeclass);
    }

    SNMALLOC_SLOW_PATH void small_dealloc_offseted_slow(
      Superslab* super, void* p, sizeclass_t sizeclass)
    {
      bool was_full = super->is_full();
      SlabList* sl = &small_classes[sizeclass];
      Slab* slab = Slab::get(p);
      Superslab::Action a =
        slab->dealloc_slow(sl, super, p, large_allocator.memory_provider);
      if (likely(a == Superslab::NoSlabReturn))
        return;
      stats().sizeclass_dealloc_slab(sizeclass);

      if (a == Superslab::NoStatusChange)
        return;

      switch (super->get_status())
      {
        case Superslab::Full:
        {
          error("Unreachable");
          break;
        }

        case Superslab::Available:
        {
          if (was_full)
          {
            super_available.insert(super);
          }
          else
          {
            super_only_short_available.remove(super);
            super_available.insert(super);
          }
          break;
        }

        case Superslab::OnlyShortSlabAvailable:
        {
          super_only_short_available.insert(super);
          break;
        }

        case Superslab::Empty:
        {
          super_available.remove(super);

          if constexpr (decommit_strategy == DecommitSuper)
          {
            large_allocator.memory_provider.notify_not_using(
              pointer_offset(super, OS_PAGE_SIZE),
              SUPERSLAB_SIZE - OS_PAGE_SIZE);
          }
          else if constexpr (decommit_strategy == DecommitSuperLazy)
          {
            static_assert(
              pal_supports<LowMemoryNotification, MemoryProvider>(),
              "A lazy decommit strategy cannot be implemented on platforms "
              "without low memory notifications");
          }

          pagemap().clear_slab(super);
          large_allocator.dealloc(super, 0);
          stats().superslab_push();
          break;
        }
      }
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    void* medium_alloc(sizeclass_t sizeclass, size_t rsize, size_t size)
    {
      MEASURE_TIME_MARKERS(
        medium_alloc,
        4,
        16,
        MARKERS(
          zero_mem == YesZero ? "zeromem" : "nozeromem",
          allow_reserve == NoReserve ? "noreserve" : "reserve"));

      sizeclass_t medium_class = sizeclass - NUM_SMALL_CLASSES;

      DLList<Mediumslab>* sc = &medium_classes[medium_class];
      Mediumslab* slab = sc->get_head();
      void* p;

      if (slab != nullptr)
      {
        p = slab->alloc<zero_mem>(size, large_allocator.memory_provider);

        if (slab->full())
          sc->pop();
      }
      else
      {
        if (void* replacement = Replacement(this))
        {
          return reinterpret_cast<Allocator*>(replacement)
            ->template medium_alloc<zero_mem, allow_reserve>(
              sizeclass, rsize, size);
        }
        slab = reinterpret_cast<Mediumslab*>(
          large_allocator.template alloc<NoZero, allow_reserve>(
            0, SUPERSLAB_SIZE));

        if ((allow_reserve == NoReserve) && (slab == nullptr))
          return nullptr;

        slab->init(public_state(), sizeclass, rsize);
        pagemap().set_slab(slab);
        p = slab->alloc<zero_mem>(size, large_allocator.memory_provider);

        if (!slab->full())
          sc->insert(slab);
      }

      stats().sizeclass_alloc(sizeclass);
      return p;
    }

    void medium_dealloc(Mediumslab* slab, void* p, sizeclass_t sizeclass)
    {
      MEASURE_TIME(medium_dealloc, 4, 16);
      stats().sizeclass_dealloc(sizeclass);
      bool was_full = slab->dealloc(p, large_allocator.memory_provider);

#ifdef CHECK_CLIENT
      if (!is_multiple_of_sizeclass(
            sizeclass_to_size(sizeclass),
            pointer_diff(p, pointer_offset(slab, SUPERSLAB_SIZE))))
      {
        error("Not deallocating start of an object");
      }
#endif

      if (slab->empty())
      {
        if (!was_full)
        {
          sizeclass_t medium_class = sizeclass - NUM_SMALL_CLASSES;
          DLList<Mediumslab>* sc = &medium_classes[medium_class];
          sc->remove(slab);
        }

        if constexpr (decommit_strategy == DecommitSuper)
        {
          large_allocator.memory_provider.notify_not_using(
            pointer_offset(slab, OS_PAGE_SIZE), SUPERSLAB_SIZE - OS_PAGE_SIZE);
        }

        pagemap().clear_slab(slab);
        large_allocator.dealloc(slab, 0);
        stats().superslab_push();
      }
      else if (was_full)
      {
        sizeclass_t medium_class = sizeclass - NUM_SMALL_CLASSES;
        DLList<Mediumslab>* sc = &medium_classes[medium_class];
        sc->insert(slab);
      }
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    void* large_alloc(size_t size)
    {
      MEASURE_TIME_MARKERS(
        large_alloc,
        4,
        16,
        MARKERS(
          zero_mem == YesZero ? "zeromem" : "nozeromem",
          allow_reserve == NoReserve ? "noreserve" : "reserve"));

      if (void* replacement = Replacement(this))
      {
        return reinterpret_cast<Allocator*>(replacement)
          ->template large_alloc<zero_mem, allow_reserve>(size);
      }

      size_t size_bits = bits::next_pow2_bits(size);
      size_t large_class = size_bits - SUPERSLAB_BITS;
      assert(large_class < NUM_LARGE_CLASSES);

      void* p = large_allocator.template alloc<zero_mem, allow_reserve>(
        large_class, size);

      pagemap().set_large_size(p, size);

      stats().large_alloc(large_class);
      return p;
    }

    void large_dealloc(void* p, size_t size)
    {
      MEASURE_TIME(large_dealloc, 4, 16);

      size_t size_bits = bits::next_pow2_bits(size);
      size_t rsize = bits::one_at_bit(size_bits);
      assert(rsize >= SUPERSLAB_SIZE);
      size_t large_class = size_bits - SUPERSLAB_BITS;

      pagemap().clear_large_size(p, size);

      stats().large_dealloc(large_class);

      // Cross-reference largealloc's alloc() decommitted condition.
      if ((decommit_strategy != DecommitNone) || (large_class > 0))
        large_allocator.memory_provider.notify_not_using(
          pointer_offset(p, OS_PAGE_SIZE), rsize - OS_PAGE_SIZE);

      // Initialise in order to set the correct SlabKind.
      Largeslab* slab = static_cast<Largeslab*>(p);
      slab->init();
      large_allocator.dealloc(slab, large_class);
    }

    // Note that this is on the slow path as it lead to better code.
    // As it is tail, not inlining means that it is jumped to, so has no perf
    // impact on the producer consumer scenarios, and doesn't require register
    // spills in the fast path for local deallocation.
    SNMALLOC_SLOW_PATH
    void remote_dealloc(RemoteAllocator* target, void* p, sizeclass_t sizeclass)
    {
      MEASURE_TIME(remote_dealloc, 4, 16);
      assert(target->id() != id());

      handle_message_queue();

      void* offseted = apply_cache_friendly_offset(p, sizeclass);

      // Check whether this will overflow the cache first.  If we are a fake
      // allocator, then our cache will always be full and so we will never hit
      // this path.
      size_t sz = sizeclass_to_size(sizeclass);
      if ((remote.size + sz) < REMOTE_CACHE)
      {
        stats().remote_free(sizeclass);
        remote.dealloc_sized(target->id(), offseted, sz);
        return;
      }
      // Now that we've established that we're in the slow path (if we're a
      // real allocator, we will have to empty our cache now), check if we are
      // a real allocator and construct one if we aren't.
      if (void* replacement = Replacement(this))
      {
        // We have to do a dealloc, not a remote_dealloc here because this may
        // have been allocated with the allocator that we've just had returned.
        reinterpret_cast<Allocator*>(replacement)->dealloc(p);
        return;
      }

      stats().remote_free(sizeclass);
      remote.dealloc(target->id(), offseted, sizeclass);

      stats().remote_post();
      remote.post(id());
    }

    PageMap& pagemap()
    {
      return page_map;
    }
  };
} // namespace snmalloc
