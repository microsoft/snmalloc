#pragma once

#if !defined(NDEBUG) && !defined(OPEN_ENCLAVE) && !defined(FreeBSD_KERNEL) && \
  !defined(USE_SNMALLOC_STATS)
#  define USE_SNMALLOC_STATS
#endif

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
  };

#ifndef SNMALLOC_MAX_FLATPAGEMAP_SIZE
// Use flat map is under a single node.
#  define SNMALLOC_MAX_FLATPAGEMAP_SIZE PAGEMAP_NODE_SIZE
#endif
  static constexpr bool USE_FLATPAGEMAP = SNMALLOC_MAX_FLATPAGEMAP_SIZE >=
    sizeof(FlatPagemap<SUPERSLAB_BITS, uint8_t>);

  using SuperslabPagemap = std::conditional_t<
    USE_FLATPAGEMAP,
    FlatPagemap<SUPERSLAB_BITS, uint8_t>,
    Pagemap<SUPERSLAB_BITS, uint8_t, 0>>;

  HEADER_GLOBAL SuperslabPagemap global_pagemap;

  /**
   * Mixin used by `SuperslabMap` to directly access the pagemap via a global
   * variable.  This should be used from within the library or program that
   * owns the pagemap.
   */
  struct GlobalPagemap
  {
    /**
     * Returns the pagemap.
     */
    SuperslabPagemap& pagemap()
    {
      return global_pagemap;
    }
  };

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
    SuperslabPagemap* external_pagemap;

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
    SuperslabPagemap& pagemap()
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
     */
    uint8_t get(address_t p)
    {
      return PagemapProvider::pagemap().get(p);
    }

    /**
     * Get the pagemap entry corresponding to a specific address.
     */
    uint8_t get(void* p)
    {
      return get(address_cast(p));
    }

    /**
     * Set a pagemap entry indicating that there is a superslab at the
     * specified index.
     */
    void set_slab(Superslab* slab)
    {
      set(slab, static_cast<size_t>(PMSuperslab));
    }
    /**
     * Add a pagemap entry indicating that a medium slab has been allocated.
     */
    void set_slab(Mediumslab* slab)
    {
      set(slab, static_cast<size_t>(PMMediumslab));
    }
    /**
     * Remove an entry from the pagemap corresponding to a superslab.
     */
    void clear_slab(Superslab* slab)
    {
      assert(get(slab) == PMSuperslab);
      set(slab, static_cast<size_t>(PMNotOurs));
    }
    /**
     * Remove an entry corresponding to a medium slab.
     */
    void clear_slab(Mediumslab* slab)
    {
      assert(get(slab) == PMMediumslab);
      set(slab, static_cast<size_t>(PMNotOurs));
    }
    /**
     * Update the pagemap to reflect a large allocation, of `size` bytes from
     * address `p`.
     */
    void set_large_size(void* p, size_t size)
    {
      size_t size_bits = bits::next_pow2_bits(size);
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
      PagemapProvider::pagemap().set(
        address_cast(p), static_cast<uint8_t>(size_bits));
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
      PagemapProvider::pagemap().set_range(p, PMNotOurs, count);
    }

  private:
    /**
     * Helper function to set a pagemap entry.  This is not part of the public
     * interface and exists to make it easy to reuse the code in the public
     * methods in other pagemap adaptors.
     */
    void set(void* p, uint8_t x)
    {
      PagemapProvider::pagemap().set(address_cast(p), x);
    }
  };

#ifndef SNMALLOC_DEFAULT_PAGEMAP
#  define SNMALLOC_DEFAULT_PAGEMAP snmalloc::SuperslabMap<>
#endif

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
   * The final template parameter, `IsQueueInline`, defines whether the
   * message queue for this allocator should be stored as a field of the
   * allocator (`true`) or provided externally, allowing it to be anywhere else
   * in the address space (`false`).
   */
  template<
    class MemoryProvider = GlobalVirtual,
    class PageMap = SNMALLOC_DEFAULT_PAGEMAP,
    bool IsQueueInline = true>
  class Allocator
  : public Pooled<Allocator<MemoryProvider, PageMap, IsQueueInline>>
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

    template<
      size_t size,
      ZeroMem zero_mem = NoZero,
      AllowReserve allow_reserve = YesReserve>
    ALLOCATOR void* alloc()
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
      constexpr uint8_t sizeclass = size_to_sizeclass_const(size);

      stats().alloc_request(size);

      handle_message_queue();

      // Allocate memory of a statically known size.
      if constexpr (sizeclass < NUM_SMALL_CLASSES)
      {
        constexpr size_t rsize = sizeclass_to_size(sizeclass);
        return small_alloc<zero_mem, allow_reserve>(sizeclass, rsize);
      }
      else if constexpr (sizeclass < NUM_SIZECLASSES)
      {
        constexpr size_t rsize = sizeclass_to_size(sizeclass);
        return medium_alloc<zero_mem, allow_reserve>(sizeclass, rsize, size);
      }
      else
      {
        return large_alloc<zero_mem, allow_reserve>(size);
      }
#endif
    }

    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    ALLOCATOR void* alloc(size_t size)
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

      handle_message_queue();

      uint8_t sizeclass = size_to_sizeclass(size);

      // Allocate memory of a dynamically known size.
      if (sizeclass < NUM_SMALL_CLASSES)
      {
        // Allocations smaller than the slab size are more likely. Improve
        // branch prediction by placing this case first.
        size_t rsize = sizeclass_to_size(sizeclass);
        return small_alloc<zero_mem, allow_reserve>(sizeclass, rsize);
      }
      if (sizeclass < NUM_SIZECLASSES)
      {
        size_t rsize = sizeclass_to_size(sizeclass);
        return medium_alloc<zero_mem, allow_reserve>(sizeclass, rsize, size);
      }

      return large_alloc<zero_mem, allow_reserve>(size);

#endif
    }

    template<size_t size>
    void dealloc(void* p)
    {
#ifdef USE_MALLOC
      UNUSED(size);
      return free(p);
#else

      constexpr uint8_t sizeclass = size_to_sizeclass_const(size);

      handle_message_queue();

      // Free memory of a statically known size. Must be called with an
      // external pointer.
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

    void dealloc(void* p, size_t size)
    {
#ifdef USE_MALLOC
      UNUSED(size);
      return free(p);
#else
      handle_message_queue();

      // Free memory of a dynamically known size. Must be called with an
      // external pointer.
      uint8_t sizeclass = size_to_sizeclass(size);

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

    void dealloc(void* p)
    {
#ifdef USE_MALLOC
      return free(p);
#else
      handle_message_queue();

      // Free memory of an unknown size. Must be called with an external
      // pointer.
      uint8_t size = pagemap().get(address_cast(p));

      if (size == 0)
      {
        error("Not allocated by this allocator");
      }

      Superslab* super = Superslab::get(p);

      if (size == PMSuperslab)
      {
        RemoteAllocator* target = super->get_allocator();
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have not yet deallocated this
        // pointer.
        uint8_t sizeclass = meta.sizeclass;

        if (super->get_allocator() == public_state())
          small_dealloc(super, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
        return;
      }
      if (size == PMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p);
        RemoteAllocator* target = slab->get_allocator();

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        uint8_t sizeclass = slab->get_sizeclass();

        if (target == public_state())
          medium_dealloc(slab, p, sizeclass);
        else
          remote_dealloc(target, p, sizeclass);
        return;
      }

#  ifndef NDEBUG
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
      uint8_t size = global_pagemap.get(address_cast(p));

      Superslab* super = Superslab::get(p);
      if (size == PMSuperslab)
      {
        Slab* slab = Slab::get(p);
        Metaslab& meta = super->get_meta(slab);

        uint8_t sc = meta.sizeclass;
        size_t slab_end = static_cast<size_t>(address_cast(slab) + SLAB_SIZE);

        return external_pointer<location>(p, sc, slab_end);
      }
      if (size == PMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p);

        uint8_t sc = slab->get_sizeclass();
        size_t slab_end =
          static_cast<size_t>(address_cast(slab) + SUPERSLAB_SIZE);

        return external_pointer<location>(p, sc, slab_end);
      }

      auto ss = address_cast(super);

      while (size > 64)
      {
        // This is a large alloc redirect.
        ss = ss - (1ULL << (size - 64));
        size = global_pagemap.get(ss);
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
      size_t size = global_pagemap.get(address_cast(p));

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

    struct RemoteList
    {
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
      size_t size = 0;
      RemoteList list[REMOTE_SLOTS];

      /// Used to find the index into the array of queues for remote
      /// deallocation
      /// r is used for which round of sending this is.
      inline size_t get_slot(size_t id, size_t r)
      {
        constexpr size_t allocator_size =
          sizeof(Allocator<MemoryProvider, PageMap, IsQueueInline>);
        constexpr size_t initial_shift =
          bits::next_pow2_bits_const(allocator_size);
        return (id >> (initial_shift + (r * REMOTE_SLOT_BITS))) & REMOTE_MASK;
      }

      void dealloc(alloc_id_t target_id, void* p, uint8_t sizeclass)
      {
        this->size += sizeclass_to_size(sizeclass);

        Remote* r = static_cast<Remote*>(p);
        r->set_target_id(target_id);
        assert(r->target_id() == target_id);

        RemoteList* l = &list[get_slot(target_id, 0)];
        l->last->non_atomic_next = r;
        l->last = r;
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
    Remote stub;

    std::conditional_t<IsQueueInline, RemoteAllocator, RemoteAllocator*>
      remote_alloc;

#ifdef CACHE_FRIENDLY_OFFSET
    size_t remote_offset = 0;

    void* apply_cache_friendly_offset(void* p, uint8_t sizeclass)
    {
      size_t mask = sizeclass_to_cache_friendly_mask(sizeclass);

      size_t offset = remote_offset & mask;
      remote_offset += CACHE_FRIENDLY_OFFSET;

      return (void*)((uintptr_t)p + offset);
    }
#else
    void* apply_cache_friendly_offset(void* p, uint8_t sizeclass)
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
      MemoryProvider& m, PageMap&& p = PageMap(), RemoteAllocator* r = nullptr)
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

      init_message_queue();
      message_queue().invariant();

#ifndef NDEBUG
      for (uint8_t i = 0; i < NUM_SIZECLASSES; i++)
      {
        size_t size = sizeclass_to_size(i);
        uint8_t sc1 = size_to_sizeclass(size);
        uint8_t sc2 = size_to_sizeclass_const(size);
        size_t size1 = sizeclass_to_size(sc1);
        size_t size2 = sizeclass_to_size(sc2);

        // All medium size classes are page aligned.
        if (i > NUM_SMALL_CLASSES)
        {
          assert(bits::is_aligned_block<OS_PAGE_SIZE>(nullptr, size1));
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
    external_pointer(void* p, uint8_t sizeclass, size_t end_point)
    {
      size_t rsize = sizeclass_to_size(sizeclass);
      size_t end_point_correction = location == End ?
        (end_point - 1) :
        (location == OnePastEnd ? end_point : (end_point - rsize));
      size_t offset_from_end =
        (end_point - 1) - static_cast<size_t>(address_cast(p));
      size_t end_to_end = round_by_sizeclass(rsize, offset_from_end);
      return end_point_correction - end_to_end;
    }

    void init_message_queue()
    {
      message_queue().init(&stub);
    }

    void handle_dealloc_remote(Remote* p)
    {
      if (p != &stub)
      {
        Superslab* super = Superslab::get(p);

        if (super->get_kind() == Super)
        {
          Slab* slab = Slab::get(p);
          Metaslab& meta = super->get_meta(slab);
          if (p->target_id() == id())
          {
            small_dealloc_offseted(super, p, meta.sizeclass);
          }
          else
          {
            // Queue for remote dealloc elsewhere.
            remote.dealloc(p->target_id(), p, meta.sizeclass);
          }
        }
        else
        {
          Mediumslab* slab = Mediumslab::get(p);
          if (p->target_id() == id())
          {
            uint8_t sizeclass = slab->get_sizeclass();
            void* start = remove_cache_friendly_offset(p, sizeclass);
            medium_dealloc(slab, start, sizeclass);
          }
          else
          {
            // Queue for remote dealloc elsewhere.
            remote.dealloc(p->target_id(), p, slab->get_sizeclass());
          }
        }
      }
    }

    NOINLINE void handle_message_queue_inner()
    {
      for (size_t i = 0; i < REMOTE_BATCH; i++)
      {
        Remote* r = message_queue().dequeue();

        if (r == nullptr)
          break;

        handle_dealloc_remote(r);
      }

      // Our remote queues may be larger due to forwarding remote frees.
      if (remote.size < REMOTE_CACHE)
        return;

      stats().remote_post();
      remote.post(id());
    }

    ALWAYSINLINE void handle_message_queue()
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
    Slab* alloc_slab(uint8_t sizeclass)
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
    void* small_alloc(uint8_t sizeclass, size_t rsize)
    {
      MEASURE_TIME_MARKERS(
        small_alloc,
        4,
        16,
        MARKERS(
          zero_mem == YesZero ? "zeromem" : "nozeromem",
          allow_reserve == NoReserve ? "noreserve" : "reserve"));

      stats().sizeclass_alloc(sizeclass);

      SlabList* sc = &small_classes[sizeclass];
      Slab* slab;

      if (!sc->is_empty())
      {
        SlabLink* link = sc->get_head();
        slab = link->get_slab();
      }
      else
      {
        slab = alloc_slab<allow_reserve>(sizeclass);

        if ((allow_reserve == NoReserve) && (slab == nullptr))
          return nullptr;

        sc->insert(slab->get_link());
      }

      return slab->alloc<zero_mem>(sc, rsize, large_allocator.memory_provider);
    }

    void small_dealloc(Superslab* super, void* p, uint8_t sizeclass)
    {
#ifndef NDEBUG
      Slab* slab = Slab::get(p);
      if (!slab->is_start_of_object(super, p))
      {
        error("Not deallocating start of an object");
      }
#endif

      void* offseted = apply_cache_friendly_offset(p, sizeclass);
      small_dealloc_offseted(super, offseted, sizeclass);
    }

    void small_dealloc_offseted(Superslab* super, void* p, uint8_t sizeclass)
    {
      MEASURE_TIME(small_dealloc, 4, 16);
      stats().sizeclass_dealloc(sizeclass);

      bool was_full = super->is_full();
      SlabList* sc = &small_classes[sizeclass];
      Slab* slab = Slab::get(p);
      Superslab::Action a =
        slab->dealloc(sc, super, p, large_allocator.memory_provider);
      if (a == Superslab::NoSlabReturn)
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
              std::remove_reference_t<decltype(
                large_allocator.memory_provider)>::
                template pal_supports<LowMemoryNotification>(),
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
    void* medium_alloc(uint8_t sizeclass, size_t rsize, size_t size)
    {
      MEASURE_TIME_MARKERS(
        medium_alloc,
        4,
        16,
        MARKERS(
          zero_mem == YesZero ? "zeromem" : "nozeromem",
          allow_reserve == NoReserve ? "noreserve" : "reserve"));

      uint8_t medium_class = sizeclass - NUM_SMALL_CLASSES;

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

    void medium_dealloc(Mediumslab* slab, void* p, uint8_t sizeclass)
    {
      MEASURE_TIME(medium_dealloc, 4, 16);
      stats().sizeclass_dealloc(sizeclass);
      bool was_full = slab->dealloc(p, large_allocator.memory_provider);

#ifndef NDEBUG
      if (!is_multiple_of_sizeclass(
            sizeclass_to_size(sizeclass),
            address_cast(slab) + SUPERSLAB_SIZE - address_cast(p)))
      {
        error("Not deallocating start of an object");
      }
#endif

      if (slab->empty())
      {
        if (!was_full)
        {
          uint8_t medium_class = sizeclass - NUM_SMALL_CLASSES;
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
        uint8_t medium_class = sizeclass - NUM_SMALL_CLASSES;
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

      if ((decommit_strategy != DecommitNone) || (large_class > 0))
        large_allocator.memory_provider.notify_not_using(
          pointer_offset(p, OS_PAGE_SIZE), rsize - OS_PAGE_SIZE);

      // Initialise in order to set the correct SlabKind.
      Largeslab* slab = static_cast<Largeslab*>(p);
      slab->init();
      large_allocator.dealloc(slab, large_class);
    }

    void remote_dealloc(RemoteAllocator* target, void* p, uint8_t sizeclass)
    {
      MEASURE_TIME(remote_dealloc, 4, 16);

      void* offseted = apply_cache_friendly_offset(p, sizeclass);

      stats().remote_free(sizeclass);
      remote.dealloc(target->id(), offseted, sizeclass);

      if (remote.size < REMOTE_CACHE)
        return;

      stats().remote_post();
      remote.post(id());
    }

    PageMap& pagemap()
    {
      return page_map;
    }
  };
} // namespace snmalloc
