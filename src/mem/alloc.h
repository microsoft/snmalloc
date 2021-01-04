#pragma once

#ifdef _MSC_VER
#  define ALLOCATOR __declspec(allocator)
#else
#  define ALLOCATOR
#endif

#include "../pal/pal_consts.h"
#include "../test/histogram.h"
#include "allocstats.h"
#include "chunkmap.h"
#include "external_alloc.h"
#include "largealloc.h"
#include "mediumslab.h"
#include "pooled.h"
#include "remoteallocator.h"
#include "sizeclasstable.h"
#include "slab.h"

#include <array>
#include <functional>

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

  /**
   * Allocator.  This class is parameterised on five template parameters.
   *
   * The first two template parameter provides a hook to allow the allocator in
   * use to be dynamically modified.  This is used to implement a trick from
   * mimalloc that avoids a conditional branch on the fast path.  We
   * initialise the thread-local allocator pointer with the address of a global
   * allocator, which never owns any memory.  The first returns true, if is
   * passed the global allocator.  The second initialises the thread-local
   * allocator if it is has been been initialised already. Splitting into two
   * functions allows for the code to be structured into tail calls to improve
   * codegen.  The second template takes a function that takes the allocator
   * that is initialised, and the value returned, is returned by
   * `InitThreadAllocator`.  This is used incase we are running during teardown
   * and the thread local allocator cannot be kept alive.
   *
   * The `MemoryProvider` defines the source of memory for this allocator.
   * Allocators try to reuse address space by allocating from existing slabs or
   * reusing freed large allocations.  When they need to allocate a new chunk
   * of memory they request space from the `MemoryProvider`.
   *
   * The `ChunkMap` parameter provides the adaptor to the pagemap.  This is used
   * to associate metadata with large (16MiB, by default) regions, allowing an
   * allocator to find the allocator responsible for that region.
   *
   * The final template parameter, `IsQueueInline`, defines whether the
   * message queue for this allocator should be stored as a field of the
   * allocator (`true`) or provided externally, allowing it to be anywhere else
   * in the address space (`false`).
   */
  template<
    bool (*NeedsInitialisation)(void*),
    ReturnPtr (*InitThreadAllocator)(function_ref<ReturnPtr(void*)>),
    class MemoryProvider = GlobalVirtual,
    class ChunkMap = SNMALLOC_DEFAULT_CHUNKMAP,
    bool IsQueueInline = true>
  class Allocator : public FastFreeLists,
                    public Pooled<Allocator<
                      NeedsInitialisation,
                      InitThreadAllocator,
                      MemoryProvider,
                      ChunkMap,
                      IsQueueInline>>
  {
    LargeAlloc<MemoryProvider> large_allocator;
    ChunkMap chunk_map;

    /**
     * Per size class bumpptr for building new free lists
     * If aligned to a SLAB start, then it is empty, and a new
     * slab is required.
     */
    AuthPtr<void> bump_ptrs[NUM_SMALL_CLASSES] = {nullptr};

  public:
    Stats& stats()
    {
      return large_allocator.stats;
    }

    template<class MP, class Alloc>
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
#ifdef SNMALLOC_PASS_THROUGH
      static_assert(
        allow_reserve == YesReserve,
        "When passing to malloc, cannot require NoResereve");
      // snmalloc guarantees a lot of alignment, so we can depend on this
      // make pass through call aligned_alloc with the alignment snmalloc
      // would guarantee.
      void* result = external_alloc::aligned_alloc(
        natural_alignment(size), round_size(size));
      if constexpr (zero_mem == YesZero)
        memset(result, 0, size);
      return result;
#else
      constexpr sizeclass_t sizeclass = size_to_sizeclass_const(size);

      stats().alloc_request(size);

      if constexpr (sizeclass < NUM_SMALL_CLASSES)
      {
        return small_alloc<zero_mem, allow_reserve>(size).unsafe_return_ptr;
      }
      else if constexpr (sizeclass < NUM_SIZECLASSES)
      {
        handle_message_queue();
        constexpr size_t rsize = sizeclass_to_size(sizeclass);
        return medium_alloc<zero_mem, allow_reserve>(sizeclass, rsize, size)
          .unsafe_return_ptr;
      }
      else
      {
        handle_message_queue();
        return large_alloc<zero_mem, allow_reserve>(size).unsafe_return_ptr;
      }
#endif
    }

    /**
     * Allocate memory of a dynamically known size.
     */
    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc(size_t size)
    {
#ifdef SNMALLOC_PASS_THROUGH
      static_assert(
        allow_reserve == YesReserve,
        "When passing to malloc, cannot require NoResereve");
      // snmalloc guarantees a lot of alignment, so we can depend on this
      // make pass through call aligned_alloc with the alignment snmalloc
      // would guarantee.
      void* result = external_alloc::aligned_alloc(
        natural_alignment(size), round_size(size));
      if constexpr (zero_mem == YesZero)
        memset(result, 0, size);
      return result;
#else
      // Perform the - 1 on size, so that zero wraps around and ends up on
      // slow path.
      if (likely((size - 1) <= (sizeclass_to_size(NUM_SMALL_CLASSES - 1) - 1)))
      {
        // Allocations smaller than the slab size are more likely. Improve
        // branch prediction by placing this case first.
        return small_alloc<zero_mem, allow_reserve>(size).unsafe_return_ptr;
      }

      return alloc_not_small<zero_mem, allow_reserve>(size).unsafe_return_ptr;
    }

    template<ZeroMem zero_mem = NoZero, AllowReserve allow_reserve = YesReserve>
    SNMALLOC_SLOW_PATH ReturnPtr alloc_not_small(size_t size)
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

    /**
     * Checks the allocation at `p` could have been validly allocated with
     * a size of `size`.
     */
    void check_size(void* p, size_t size)
    {
#if defined(CHECK_CLIENT)
      auto asize = alloc_size(p);
      auto asc = size_to_sizeclass(asize);
      if (size_to_sizeclass(size) != asc)
      {
        // Correction for large classes.
        if (asc > NUM_SIZECLASSES)
        {
          if (bits::next_pow2(size) != asize)
            error("Deallocating with incorrect size supplied.");
        }
        // Correction for zero sized allocations.
        else if ((size != 0) && (asc != 0))
        {
          error("Deallocating with incorrect size supplied.");
        }
      }
#else
      UNUSED(p);
      UNUSED(size);
#endif
    }

    /*
     * Free memory of a statically known size. Must be called with an
     * external pointer.
     */
    template<size_t size>
    void dealloc(void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      UNUSED(size);
      return external_alloc::free(p_raw);
#else
      check_size(p_raw, size);
      constexpr sizeclass_t sizeclass = size_to_sizeclass_const(size);

      ReturnPtr p_ret = unsafe_as_returnptr(p_raw);
      AuthPtr<void> p_auth = mk_authptr(p_raw);
      FreePtr<void> p_free = unsafe_as_freeptr<void>(p_ret);

      if (sizeclass < NUM_SMALL_CLASSES)
      {
        Superslab* super = Superslab::get(p_auth);
        RemoteAllocator* target = super->get_allocator();

        if (likely(target == public_state()))
          small_dealloc(super, p_auth, p_free, sizeclass);
        else
          remote_dealloc(target, p_free, sizeclass);
      }
      else if (sizeclass < NUM_SIZECLASSES)
      {
        Mediumslab* slab = Mediumslab::get(p_auth);
        RemoteAllocator* target = slab->get_allocator();

        if (likely(target == public_state()))
          medium_dealloc(slab, p_free, sizeclass);
        else
          remote_dealloc(target, p_free, sizeclass);
      }
      else
      {
        large_dealloc(p_auth, p_free, size);
      }
#endif
    }

    /*
     * Free memory of a dynamically known size. Must be called with an
     * external pointer.
     */
    SNMALLOC_FAST_PATH void dealloc(void* p_raw, size_t size)
    {
#ifdef SNMALLOC_PASS_THROUGH
      UNUSED(size);
      return external_alloc::free(p_raw);
#else
      SNMALLOC_ASSERT(p_raw != nullptr);
      check_size(p_raw, size);

      ReturnPtr p_ret = unsafe_as_returnptr(p_raw);
      AuthPtr<void> p_auth = mk_authptr(p_raw);
      FreePtr<void> p_free = unsafe_as_freeptr<void>(p_ret);

      if (likely((size - 1) <= (sizeclass_to_size(NUM_SMALL_CLASSES - 1) - 1)))
      {
        Superslab* super = Superslab::get(p_auth);
        RemoteAllocator* target = super->get_allocator();
        sizeclass_t sizeclass = size_to_sizeclass(size);
        if (likely(target == public_state()))
          small_dealloc(super, p_auth, p_free, sizeclass);
        else
          remote_dealloc(target, p_free, sizeclass);
        return;
      }
      dealloc_sized_slow(p_auth, p_free, size);
#endif
    }

    SNMALLOC_SLOW_PATH
    void
    dealloc_sized_slow(AuthPtr<void> p_auth, FreePtr<void> p_free, size_t size)
    {
      if (size == 0)
        return dealloc(p_free.unsafe_free_ptr, 1);

      if (likely(size <= sizeclass_to_size(NUM_SIZECLASSES - 1)))
      {
        Mediumslab* slab = Mediumslab::get(p_auth);
        RemoteAllocator* target = slab->get_allocator();
        sizeclass_t sizeclass = size_to_sizeclass(size);
        if (likely(target == public_state()))
          medium_dealloc(slab, p_free, sizeclass);
        else
          remote_dealloc(target, p_free, sizeclass);
        return;
      }
      large_dealloc(p_auth, p_free, size);
    }

    /*
     * Free memory of an unknown size. Must be called with an external
     * pointer.
     */
    SNMALLOC_FAST_PATH void dealloc(void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      return external_alloc::free(p_raw);
#else

      ReturnPtr p_ret = unsafe_as_returnptr(p_raw);
      uint8_t size = chunkmap().get(p_ret);
      AuthPtr<void> p_auth = mk_authptr(p_raw);
      FreePtr<void> p_free = unsafe_as_freeptr<void>(p_ret);

      if (likely(size == CMSuperslab))
      {
        Superslab* super = Superslab::get(p_auth);
        RemoteAllocator* target = super->get_allocator();
        Slab* slab = Metaslab::get_slab(p_auth);
        Metaslab& meta = super->get_meta(slab);

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have not yet deallocated this
        // pointer.
        sizeclass_t sizeclass = meta.sizeclass;

        if (likely(super->get_allocator() == public_state()))
          small_dealloc(super, p_auth, p_free, sizeclass);
        else
          remote_dealloc(target, p_free, sizeclass);
        return;
      }
      dealloc_not_small(p_auth, p_free, size);
    }

    SNMALLOC_SLOW_PATH
    void
    dealloc_not_small(AuthPtr<void> p_auth, FreePtr<void> p_free, uint8_t size)
    {
      handle_message_queue();

      if (p_free == nullptr)
        return;

      if (size == CMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p_auth);
        RemoteAllocator* target = slab->get_allocator();

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have not yet deallocated this pointer.
        sizeclass_t sizeclass = slab->get_sizeclass();

        if (target == public_state())
          medium_dealloc(slab, p_free, sizeclass);
        else
          remote_dealloc(target, p_free, sizeclass);
        return;
      }

      if (size == 0)
      {
        error("Not allocated by this allocator");
      }

#  ifdef CHECK_CLIENT
      Superslab* super = Superslab::get(p_auth);
      if (size > CMLargeMax || address_cast(super) != address_cast(p_auth))
      {
        error("Not deallocating start of an object");
      }
#  endif
      large_dealloc(p_auth, p_free, 1ULL << size);

#endif
    }

    template<Boundary location = Start>
    void* external_pointer(void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      error("Unsupported");
      UNUSED(p_raw);
#else
      ReturnPtr p_ret = unsafe_as_returnptr(p_raw);
      uint8_t size = chunkmap().get(p_ret);
      AuthPtr<void> p_auth = mk_authptr(p_raw);

      Superslab* super = Superslab::get(p_auth);
      if (size == CMSuperslab)
      {
        Slab* slab = Metaslab::get_slab(p_auth);
        Metaslab& meta = super->get_meta(slab);

        sizeclass_t sc = meta.sizeclass;
        void* slab_end = pointer_offset(slab, SLAB_SIZE);

        return external_pointer<location>(p_ret, sc, slab_end)
          .unsafe_return_ptr;
      }
      if (size == CMMediumslab)
      {
        Mediumslab* slab = Mediumslab::get(p_auth);

        sizeclass_t sc = slab->get_sizeclass();
        void* slab_end = pointer_offset(slab, SUPERSLAB_SIZE);

        return external_pointer<location>(p_ret, sc, slab_end)
          .unsafe_return_ptr;
      }

      auto ss = super;

      while (size >= CMLargeRangeMin)
      {
        // This is a large alloc redirect.
        ss = pointer_offset_signed(
          ss,
          -(static_cast<ptrdiff_t>(1)
            << (size - CMLargeRangeMin + SUPERSLAB_BITS)));
        size = chunkmap().get(unsafe_as_returnptr(ss));
      }

      if (size == 0)
      {
        if constexpr ((location == End) || (location == OnePastEnd))
          // We don't know the End, so return MAX_PTR
          return pointer_offset<void>(nullptr, UINTPTR_MAX);
        else
          // We don't know the Start, so return MIN_PTR
          return nullptr;
      }

      // This is a large alloc, mask off to the slab size.
      if constexpr (location == Start)
        return ss;
      else if constexpr (location == End)
        return pointer_offset(ss, (1ULL << size) - 1ULL);
      else
        return pointer_offset(ss, 1ULL << size);
#endif
    }

  private:
    SNMALLOC_SLOW_PATH static size_t alloc_size_error()
    {
      error("Not allocated by this allocator");
    }

  public:
    SNMALLOC_FAST_PATH size_t alloc_size(const void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      return external_alloc::malloc_usable_size(const_cast<void*>(p_raw));
#else
      // This must be called on an external pointer.
      ReturnPtr p_ret = unsafe_as_returnptr(const_cast<void*>(p_raw));
      size_t size = chunkmap().get(p_ret);
      AuthPtr<void> p_auth = mk_authptr(const_cast<void*>(p_raw));

      if (likely(size == CMSuperslab))
      {
        Superslab* super = Superslab::get(p_auth);

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        Slab* slab = Metaslab::get_slab(p_auth);
        Metaslab& meta = super->get_meta(slab);

        return sizeclass_to_size(meta.sizeclass);
      }

      if (likely(size == CMMediumslab))
      {
        Mediumslab* slab = Mediumslab::get(p_auth);
        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        return sizeclass_to_size(slab->get_sizeclass());
      }

      if (likely(size != 0))
      {
        return 1ULL << size;
      }

      return alloc_size_error();
#endif
    }

    /**
     * Return this allocator's "truncated" ID, an integer useful as a hash
     * value of this allocator.
     *
     * Specifically, this is the address of this allocator's message queue
     * with the least significant bits missing, masked by SIZECLASS_MASK.
     * This will be unique for Allocs with inline queues; Allocs with
     * out-of-line queues must ensure that no two queues' addresses collide
     * under this masking.
     */
    size_t get_trunc_id()
    {
      return public_state()->trunc_id();
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
      Remote head{};

      FreePtr<Remote> last{unsafe_mk_freeptr<Remote>(mk_authptr(&head))};

      void clear()
      {
        last = unsafe_mk_freeptr<Remote>(mk_authptr(&head));
      }

      bool empty()
      {
        return last.unsafe_free_ptr == &head;
      }
    };

    struct RemoteCache
    {
      /**
       * The total amount of memory we are waiting for before we will dispatch
       * to other allocators. Zero or negative mean we should dispatch on the
       * next remote deallocation. This is initialised to the 0 so that we
       * always hit a slow path to start with, when we hit the slow path and
       * need to dispatch everything, we can check if we are a real allocator
       * and lazily provide a real allocator.
       */
      int64_t capacity{0};
      std::array<RemoteList, REMOTE_SLOTS> list{};

      /// Used to find the index into the array of queues for remote
      /// deallocation
      /// r is used for which round of sending this is.
      inline size_t get_slot(size_t id, size_t r)
      {
        constexpr size_t allocator_size = sizeof(Allocator<
                                                 NeedsInitialisation,
                                                 InitThreadAllocator,
                                                 MemoryProvider,
                                                 ChunkMap,
                                                 IsQueueInline>);
        constexpr size_t initial_shift =
          bits::next_pow2_bits_const(allocator_size);
        static_assert(
          initial_shift >= 8,
          "Can't embed sizeclass_t into allocator ID low bits");
        SNMALLOC_ASSERT((initial_shift + (r * REMOTE_SLOT_BITS)) < 64);
        return (id >> (initial_shift + (r * REMOTE_SLOT_BITS))) & REMOTE_MASK;
      }

      SNMALLOC_FAST_PATH void dealloc(
        alloc_id_t target_id, FreePtr<Remote> p_free, sizeclass_t sizeclass)
      {
        this->capacity -= sizeclass_to_size(sizeclass);

        Remote* r = p_free.unsafe_free_ptr;
        r->set_info(target_id, sizeclass);

        RemoteList* l = &list[get_slot(target_id, 0)];
        l->last.unsafe_free_ptr->non_atomic_next = p_free;
        l->last = p_free;
      }

      void post(LargeAlloc<MemoryProvider>* large_allocator, alloc_id_t id)
      {
        UNUSED(large_allocator);

        // When the cache gets big, post lists to their target allocators.
        capacity = REMOTE_CACHE;

        size_t post_round = 0;

        while (true)
        {
          auto my_slot = get_slot(id, post_round);

          for (size_t i = 0; i < REMOTE_SLOTS; i++)
          {
            if (i == my_slot)
              continue;

            RemoteList* l = &list[i];
            FreePtr<Remote> first = l->head.non_atomic_next;

            if (!l->empty())
            {
              // Send all slots to the target at the head of the list.
              AuthPtr<void> first_auth = mk_authptr(first.unsafe_free_ptr);
              /*
               * This is somewhat dubious: this chunk might be either a
               * Superslab or a Mediumslab, but we access only the
               * get_allocator() method of their common parent class,
               * Allocslab, which reads the allocator field, which should
               * have common offset.
               */
#ifdef __clang__
              static_assert(
                offsetof(Superslab, allocator) ==
                  offsetof(Mediumslab, allocator),
                "Allocslab derived classes have differing allocator offsets");
#endif
              Superslab* super = Superslab::get(first_auth);
              super->get_allocator()->message_queue.enqueue(
                first.unsafe_free_ptr, l->last.unsafe_free_ptr);
              l->clear();
            }
          }

          RemoteList* resend = &list[my_slot];
          if (resend->empty())
            break;

          // Entries could map back onto the "resend" list,
          // so take copy of the head, mark the last element,
          // and clear the original list.
          FreePtr<Remote> r = resend->head.non_atomic_next;
          resend->last.unsafe_free_ptr->non_atomic_next = nullptr;
          resend->clear();

          post_round++;

          while (r != nullptr)
          {
            // Use the next N bits to spread out remote deallocs in our own
            // slot.
            size_t slot =
              get_slot(r.unsafe_free_ptr->trunc_target_id(), post_round);
            RemoteList* l = &list[slot];
            l->last.unsafe_free_ptr->non_atomic_next = r;
            l->last = r;

            r = r.unsafe_free_ptr->non_atomic_next;
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

    template<typename T>
    FreePtr<T>
    apply_cache_friendly_offset(FreePtr<void> p, sizeclass_t sizeclass)
    {
      size_t mask = sizeclass_to_cache_friendly_mask(sizeclass);

      size_t offset = remote_offset & mask;
      remote_offset += CACHE_FRIENDLY_OFFSET;

      return unsafe_mk_freeptr<T>(
        mk_authptr(pointer_offset(p.unsafe_free_ptr, offset)));
    }
#else
    template<typename T>
    FreePtr<T>
    apply_cache_friendly_offset(FreePtr<void> p, sizeclass_t sizeclass)
    {
      UNUSED(sizeclass);
      return static_cast<FreePtr<T>>(p);
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

    auto& message_queue()
    {
      return public_state()->message_queue;
    }

    template<class A, class MemProvider>
    friend class Pool;

  public:
    Allocator(
      MemoryProvider& m,
      ChunkMap&& c = ChunkMap(),
      RemoteAllocator* r = nullptr,
      bool isFake = false)
    : large_allocator(m), chunk_map(c)
    {
      if constexpr (IsQueueInline)
      {
        SNMALLOC_ASSERT(r == nullptr);
        (void)r;
      }
      else
      {
        remote_alloc = r;
      }

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

        SNMALLOC_ASSERT(sc1 == i);
        SNMALLOC_ASSERT(sc1 == sc2);
        SNMALLOC_ASSERT(size1 == size);
        SNMALLOC_ASSERT(size1 == size2);
      }
#endif
    }

    /**
     * If result parameter is non-null, then false is assigned into the
     * the location pointed to by result if this allocator is non-empty.
     *
     * If result pointer is null, then this code raises a Pal::error on the
     * particular check that fails, if any do fail.
     */
    void debug_is_empty(bool* result)
    {
      auto test = [&result](auto& queue) {
        if (!queue.is_empty())
        {
          if (result != nullptr)
            *result = false;
          else
            error("debug_is_empty: found non-empty allocator");
        }
      };

      // Destroy the message queue so that it has no stub message.
      {
        auto p =
          unsafe_mk_freeptr<Remote>(mk_authptr(message_queue().destroy()));

        while (p != nullptr)
        {
          FreePtr<Remote> n = p.unsafe_free_ptr->non_atomic_next;
          handle_dealloc_remote(p);
          p = n;
        }
      }

      // Dump bump allocators back into memory
      for (size_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        auto& bp = bump_ptrs[i];
        auto rsize = sizeclass_to_size(i);
        FreeListHead ffl;
        while (pointer_align_up(bp, SLAB_SIZE) != bp)
        {
          Slab::alloc_new_list(bp, ffl, rsize);
          FreePtr<FreeListEntry> prev = ffl.value;
          while (prev != nullptr)
          {
            auto n = Metaslab::follow_next(prev);
            AuthPtr<void> prev_auth = mk_authptr(prev.unsafe_free_ptr);
            Superslab* super = Superslab::get(prev_auth);
            Slab* slab = Metaslab::get_slab(prev_auth);
            FreePtr<FreeListEntry> prev_free =
              unsafe_mk_freeptr<FreeListEntry>(prev_auth);
            small_dealloc_offseted_inner(super, slab, prev_free, i);
            prev = n;
          }
        }
      }

      for (size_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        auto prev = small_fast_free_lists[i].value;
        small_fast_free_lists[i].value = nullptr;
        while (prev != nullptr)
        {
          auto n = Metaslab::follow_next(prev);

          AuthPtr<void> prev_auth = mk_authptr(prev.unsafe_free_ptr);
          Superslab* super = Superslab::get(prev_auth);
          Slab* slab = Metaslab::get_slab(prev_auth);
          FreePtr<FreeListEntry> prev_free =
            unsafe_mk_freeptr<FreeListEntry>(prev_auth);
          small_dealloc_offseted_inner(super, slab, prev_free, i);

          prev = n;
        }

        test(small_classes[i]);
      }

      for (auto& medium_class : medium_classes)
      {
        test(medium_class);
      }

      test(super_available);
      test(super_only_short_available);

      // Place the static stub message on the queue.
      init_message_queue();
    }

    template<Boundary location>
    static ReturnPtr
    external_pointer(ReturnPtr p, sizeclass_t sizeclass, void* end_point)
    {
      size_t rsize = sizeclass_to_size(sizeclass);

      void* end_point_correction = location == End ?
        pointer_offset_signed(end_point, -1) :
        (location == OnePastEnd ?
           end_point :
           pointer_offset_signed(end_point, -static_cast<ptrdiff_t>(rsize)));

      size_t offset_from_end =
        pointer_diff(p.unsafe_return_ptr, pointer_offset_signed(end_point, -1));

      size_t end_to_end = round_by_sizeclass(rsize, offset_from_end);

      return unsafe_as_returnptr(pointer_offset_signed(
        end_point_correction, -static_cast<ptrdiff_t>(end_to_end)));
    }

    void init_message_queue()
    {
      // Manufacture an allocation to prime the queue
      // Using an actual allocation removes a conditional from a critical path.
      Remote* dummy = reinterpret_cast<Remote*>(alloc<YesZero>(MIN_ALLOC_SIZE));
      if (dummy == nullptr)
      {
        error("Critical error: Out-of-memory during initialisation.");
      }
      dummy->set_info(get_trunc_id(), size_to_sizeclass_const(MIN_ALLOC_SIZE));
      message_queue().init(dummy);
    }

    SNMALLOC_FAST_PATH void handle_dealloc_remote(FreePtr<Remote> p_free)
    {
      Remote* r = p_free.unsafe_free_ptr;
      sizeclass_t psz = r->sizeclass();
      alloc_id_t pid = r->trunc_target_id();

      if (likely(pid == get_trunc_id()))
      {
        // Destined for my slabs
        AuthPtr<void> p_auth = mk_authptr(p_free.unsafe_free_ptr);
        Superslab* super = Superslab::get(p_auth);

#ifdef CHECK_CLIENT
        if (pid != (super->get_allocator()->trunc_id()))
          error("Detected memory corruption.  Potential use-after-free");
#endif
        // Guard against remote queues that have colliding IDs
        SNMALLOC_ASSERT(super->get_allocator() == public_state());

        /*
         * Zero out the Remote pointers before pushing this entry to the
         * backing regions' free lists.  Notably, Mediumslabs will preserve
         * the whole allocation as zero while Slabs will chain this onto a
         * free list.
         */
        FreePtr<FreeListEntry> fpf = zero_remote<FreeListEntry>(p_free);

        if (likely(psz < NUM_SMALL_CLASSES))
        {
          SNMALLOC_ASSERT(super->get_kind() == Super);
          Slab* slab = Metaslab::get_slab(p_auth);
          small_dealloc_offseted(super, slab, fpf, psz);
        }
        else
        {
          SNMALLOC_ASSERT(super->get_kind() == Medium);
          FreePtr<void> start = remove_cache_friendly_offset(fpf, psz);
          medium_dealloc(Mediumslab::get(p_auth), start, psz);
        }
      }
      else
      {
        // Merely routing
        remote.dealloc(pid, p_free, psz);
      }
    }

    SNMALLOC_SLOW_PATH void handle_message_queue_inner()
    {
      for (size_t i = 0; i < REMOTE_BATCH; i++)
      {
        auto r = message_queue().dequeue();

        if (unlikely(!r.second))
          break;

        handle_dealloc_remote(unsafe_mk_freeptr<Remote>(mk_authptr(r.first)));
      }

      // Our remote queues may be larger due to forwarding remote frees.
      if (likely(remote.capacity > 0))
        return;

      stats().remote_post();
      remote.post(&large_allocator, get_trunc_id());
    }

    /**
     * Check if this allocator has messages to deallocate blocks from another
     * thread
     */
    SNMALLOC_FAST_PATH bool has_messages()
    {
      return !(message_queue().is_empty());
    }

    SNMALLOC_FAST_PATH void handle_message_queue()
    {
      // Inline the empty check, but not necessarily the full queue handling.
      if (likely(!has_messages()))
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

      if (super == nullptr)
        return super;

      super->init(public_state());
      chunkmap().set_slab(super);
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
    SNMALLOC_SLOW_PATH Slab* alloc_slab(sizeclass_t sizeclass)
    {
      stats().sizeclass_alloc_slab(sizeclass);
      if (Superslab::is_short_sizeclass(sizeclass))
      {
        // Pull a short slab from the list of superslabs that have only the
        // short slab available.
        Superslab* super = super_only_short_available.pop();

        if (super != nullptr)
        {
          Slab* slab = super->alloc_short_slab(sizeclass);
          SNMALLOC_ASSERT(super->is_full());
          return slab;
        }

        super = get_superslab<allow_reserve>();

        if (super == nullptr)
          return nullptr;

        Slab* slab = super->alloc_short_slab(sizeclass);
        reposition_superslab(super);
        return slab;
      }

      Superslab* super = get_superslab<allow_reserve>();

      if (super == nullptr)
        return nullptr;

      Slab* slab = super->alloc_slab(sizeclass);
      reposition_superslab(super);
      return slab;
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_FAST_PATH ReturnPtr small_alloc(size_t size)
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
      return small_alloc_inner<zero_mem, allow_reserve>(sizeclass, size);
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_FAST_PATH ReturnPtr
    small_alloc_inner(sizeclass_t sizeclass, size_t size)
    {
      SNMALLOC_ASSUME(sizeclass < NUM_SMALL_CLASSES);
      auto& fl = small_fast_free_lists[sizeclass];

      FreePtr<FreeListEntry> head = fl.value;

      if (likely(head != nullptr))
      {
        stats().alloc_request(size);
        stats().sizeclass_alloc(sizeclass);
        // Read the next slot from the memory that's about to be allocated.

        fl.value = Metaslab::follow_next(head);

        FreePtr<void> p = remove_cache_friendly_offset(head, sizeclass);
        if constexpr (zero_mem == YesZero)
        {
          MemoryProvider::Pal::zero(
            p.unsafe_free_ptr, sizeclass_to_size(sizeclass));
        }
        return unsafe_mk_returnptr(p);
      }

      if (likely(!has_messages()))
        return small_alloc_next_free_list<zero_mem, allow_reserve>(
          sizeclass, size);

      return small_alloc_mq_slow<zero_mem, allow_reserve>(sizeclass, size);
    }

    /**
     * Slow path for handling message queue, before dealing with small
     * allocation request.
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_SLOW_PATH ReturnPtr
    small_alloc_mq_slow(sizeclass_t sizeclass, size_t size)
    {
      handle_message_queue_inner();

      return small_alloc_next_free_list<zero_mem, allow_reserve>(
        sizeclass, size);
    }

    /**
     * Attempt to find a new free list to allocate from
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_SLOW_PATH ReturnPtr
    small_alloc_next_free_list(sizeclass_t sizeclass, size_t size)
    {
      size_t rsize = sizeclass_to_size(sizeclass);
      auto& sl = small_classes[sizeclass];

      Slab* slab;

      if (likely(!sl.is_empty()))
      {
        stats().alloc_request(size);
        stats().sizeclass_alloc(sizeclass);

        SlabLink* link = sl.get_next();
        slab = get_slab(link);
        auto& ffl = small_fast_free_lists[sizeclass];
        return slab->alloc<zero_mem, typename MemoryProvider::Pal>(
          sl, ffl, rsize);
      }
      return small_alloc_rare<zero_mem, allow_reserve>(sizeclass, size);
    }

    /**
     * Called when there are no available free list to service this request
     * Could be due to using the dummy allocator, or needing to bump allocate a
     * new free list.
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_SLOW_PATH ReturnPtr
    small_alloc_rare(sizeclass_t sizeclass, size_t size)
    {
      if (likely(!NeedsInitialisation(this)))
      {
        stats().alloc_request(size);
        stats().sizeclass_alloc(sizeclass);
        return small_alloc_new_free_list<zero_mem, allow_reserve>(sizeclass);
      }
      return small_alloc_first_alloc<zero_mem, allow_reserve>(sizeclass, size);
    }

    /**
     * Called on first allocation to set up the thread local allocator,
     * then directs the allocation request to the newly created allocator.
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_SLOW_PATH ReturnPtr
    small_alloc_first_alloc(sizeclass_t sizeclass, size_t size)
    {
      return InitThreadAllocator([sizeclass, size](void* alloc) {
        return reinterpret_cast<Allocator*>(alloc)
          ->template small_alloc_inner<zero_mem, allow_reserve>(
            sizeclass, size);
      });
    }

    /**
     * Called to create a new free list, and service the request from that new
     * list.
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_FAST_PATH ReturnPtr
    small_alloc_new_free_list(sizeclass_t sizeclass)
    {
      auto& bp = bump_ptrs[sizeclass];
      if (likely(pointer_align_up(bp, SLAB_SIZE) != bp))
      {
        return small_alloc_build_free_list<zero_mem, allow_reserve>(sizeclass);
      }
      // Fetch new slab
      return small_alloc_new_slab<zero_mem, allow_reserve>(sizeclass);
    }

    /**
     * Creates a new free list from the thread local bump allocator and service
     * the request from that new list.
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_FAST_PATH ReturnPtr
    small_alloc_build_free_list(sizeclass_t sizeclass)
    {
      auto& bp = bump_ptrs[sizeclass];
      auto rsize = sizeclass_to_size(sizeclass);
      auto& ffl = small_fast_free_lists[sizeclass];
      SNMALLOC_ASSERT(ffl.value == nullptr);
      Slab::alloc_new_list(bp, ffl, rsize);

      FreePtr<void> p = remove_cache_friendly_offset(ffl.value, sizeclass);
      ffl.value = Metaslab::follow_next(ffl.value);

      if constexpr (zero_mem == YesZero)
      {
        MemoryProvider::Pal::zero(
          p.unsafe_free_ptr, sizeclass_to_size(sizeclass));
      }
      return unsafe_mk_returnptr(p);
    }

    /**
     * Allocates a new slab to allocate from, set it to be the bump allocator
     * for this size class, and then builds a new free list from the thread
     * local bump allocator and service the request from that new list.
     */
    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    SNMALLOC_SLOW_PATH ReturnPtr small_alloc_new_slab(sizeclass_t sizeclass)
    {
      auto& bp = bump_ptrs[sizeclass];
      // Fetch new slab
      Slab* slab = alloc_slab<allow_reserve>(sizeclass);
      if (slab == nullptr)
        return nullptr;
      bp = mk_authptr(
        pointer_offset(slab, get_initial_offset(sizeclass, slab->is_short())));

      return small_alloc_build_free_list<zero_mem, allow_reserve>(sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc(
      Superslab* super,
      AuthPtr<void> p_auth,
      FreePtr<void> p_free,
      sizeclass_t sizeclass)
    {
      Slab* slab = Metaslab::get_slab(p_auth);
#ifdef CHECK_CLIENT
      if (!slab->is_start_of_object(super, p_free.unsafe_free_ptr))
      {
        error("Not deallocating start of an object");
      }
#endif

      FreePtr<FreeListEntry> offseted =
        apply_cache_friendly_offset<FreeListEntry>(p_free, sizeclass);
      small_dealloc_offseted(super, slab, offseted, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_offseted(
      Superslab* super,
      Slab* slab,
      FreePtr<FreeListEntry> p_free,
      sizeclass_t sizeclass)
    {
      MEASURE_TIME(small_dealloc, 4, 16);
      stats().sizeclass_dealloc(sizeclass);

      small_dealloc_offseted_inner(super, slab, p_free, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_offseted_inner(
      Superslab* super,
      Slab* slab,
      FreePtr<FreeListEntry> p_free,
      sizeclass_t sizeclass)
    {
      if (likely(slab->dealloc_fast(super, p_free)))
        return;

      small_dealloc_offseted_slow(super, slab, p_free, sizeclass);
    }

    SNMALLOC_SLOW_PATH void small_dealloc_offseted_slow(
      Superslab* super,
      Slab* slab,
      FreePtr<FreeListEntry> p_free,
      sizeclass_t sizeclass)
    {
      bool was_full = super->is_full();
      SlabList* sl = &small_classes[sizeclass];
      Superslab::Action a = slab->dealloc_slow(sl, super, p_free);
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

          chunkmap().clear_slab(super);

          auto super_auth = mk_authptr(super);
          auto super_free = unsafe_mk_freeptr<void>(super_auth);

          large_allocator.dealloc(super_auth, super_free, 0);
          stats().superslab_push();
          break;
        }
      }
    }

    template<ZeroMem zero_mem, AllowReserve allow_reserve>
    ReturnPtr medium_alloc(sizeclass_t sizeclass, size_t rsize, size_t size)
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
      FreePtr<void> p = nullptr;

      if (slab != nullptr)
      {
        p = slab->alloc<zero_mem, typename MemoryProvider::Pal>(size);

        if (slab->full())
          sc->pop();
      }
      else
      {
        if (NeedsInitialisation(this))
        {
          return InitThreadAllocator([size, rsize, sizeclass](void* alloc) {
            return reinterpret_cast<Allocator*>(alloc)
              ->medium_alloc<zero_mem, allow_reserve>(sizeclass, rsize, size);
          });
        }
        slab = reinterpret_cast<Mediumslab*>(
          large_allocator.template alloc<NoZero, allow_reserve>(
            0, SUPERSLAB_SIZE));

        if (slab == nullptr)
          return nullptr;

        slab->init(public_state(), sizeclass, rsize);
        chunkmap().set_slab(slab);
        p = slab->alloc<zero_mem, typename MemoryProvider::Pal>(size);

        if (!slab->full())
          sc->insert(slab);
      }

      stats().alloc_request(size);
      stats().sizeclass_alloc(sizeclass);
      // XXX
      return unsafe_mk_returnptr(p);
    }

    void medium_dealloc(
      Mediumslab* slab, FreePtr<void> p_free, sizeclass_t sizeclass)
    {
      MEASURE_TIME(medium_dealloc, 4, 16);
      stats().sizeclass_dealloc(sizeclass);
      bool was_full = slab->dealloc(p_free);

#ifdef CHECK_CLIENT
      if (!is_multiple_of_sizeclass(
            sizeclass_to_size(sizeclass),
            pointer_diff(
              p_free.unsafe_free_ptr, pointer_offset(slab, SUPERSLAB_SIZE))))
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

        chunkmap().clear_slab(slab);

        auto slab_auth = mk_authptr(slab);
        auto slab_free = unsafe_mk_freeptr<void>(slab_auth);

        large_allocator.dealloc(slab_auth, slab_free, 0);
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
    ReturnPtr large_alloc(size_t size)
    {
      MEASURE_TIME_MARKERS(
        large_alloc,
        4,
        16,
        MARKERS(
          zero_mem == YesZero ? "zeromem" : "nozeromem",
          allow_reserve == NoReserve ? "noreserve" : "reserve"));

      if (NeedsInitialisation(this))
      {
        return InitThreadAllocator([size](void* alloc) {
          return reinterpret_cast<Allocator*>(alloc)
            ->large_alloc<zero_mem, allow_reserve>(size);
        });
      }

      size_t size_bits = bits::next_pow2_bits(size);
      size_t large_class = size_bits - SUPERSLAB_BITS;
      SNMALLOC_ASSERT(large_class < NUM_LARGE_CLASSES);

      void* p = large_allocator.template alloc<zero_mem, allow_reserve>(
        large_class, size);
      if (likely(p != nullptr))
      {
        chunkmap().set_large_size(p, size);

        stats().alloc_request(size);
        stats().large_alloc(large_class);
      }
      // XXX
      return unsafe_mk_returnptr(
        Aal::template ptrauth_bound<void>(mk_authptr<void>(p), size));
    }

    /*
     * Takes both an AuthPtr to prove to the OS that it can do page
     * manipulations and a FreePtr to put into the free list.
     */
    void large_dealloc(AuthPtr<void> p_auth, FreePtr<void> p_free, size_t size)
    {
      MEASURE_TIME(large_dealloc, 4, 16);

      if (NeedsInitialisation(this))
      {
        InitThreadAllocator([p_auth, p_free, size](void* alloc) {
          reinterpret_cast<Allocator*>(alloc)->large_dealloc(
            p_auth, p_free, size);
          return nullptr;
        });
        return;
      }

      size_t size_bits = bits::next_pow2_bits(size);
      SNMALLOC_ASSERT(bits::one_at_bit(size_bits) >= SUPERSLAB_SIZE);
      size_t large_class = size_bits - SUPERSLAB_BITS;

      chunkmap().clear_large_size(p_auth.unsafe_auth_ptr, size);

      stats().large_dealloc(large_class);

      // Initialise in order to set the correct SlabKind.
      Largeslab* slab = static_cast<Largeslab*>(p_free.unsafe_free_ptr);
      slab->init();
      large_allocator.dealloc(p_auth, p_free, large_class);
    }

    // This is still considered the fast path as all the complex code is tail
    // called in its slow path. This leads to one fewer unconditional jump in
    // Clang.
    SNMALLOC_FAST_PATH
    void remote_dealloc(
      RemoteAllocator* target, FreePtr<void> p_free, sizeclass_t sizeclass)
    {
      MEASURE_TIME(remote_dealloc, 4, 16);
      SNMALLOC_ASSERT(target->trunc_id() != get_trunc_id());

      // Check whether this will overflow the cache first.  If we are a fake
      // allocator, then our cache will always be full and so we will never hit
      // this path.
      if (remote.capacity > 0)
      {
        FreePtr<Remote> offseted =
          apply_cache_friendly_offset<Remote>(p_free, sizeclass);
        stats().remote_free(sizeclass);
        remote.dealloc(target->trunc_id(), offseted, sizeclass);
        return;
      }

      remote_dealloc_slow(target, p_free, sizeclass);
    }

    SNMALLOC_SLOW_PATH void remote_dealloc_slow(
      RemoteAllocator* target, FreePtr<void> p_free, sizeclass_t sizeclass)
    {
      SNMALLOC_ASSERT(target->trunc_id() != get_trunc_id());

      // Now that we've established that we're in the slow path (if we're a
      // real allocator, we will have to empty our cache now), check if we are
      // a real allocator and construct one if we aren't.
      if (NeedsInitialisation(this))
      {
        InitThreadAllocator([p_free](void* alloc) {
          reinterpret_cast<Allocator*>(alloc)->dealloc(p_free.unsafe_free_ptr);
          return nullptr;
        });
        return;
      }

      handle_message_queue();

      stats().remote_free(sizeclass);
      FreePtr<Remote> offseted =
        apply_cache_friendly_offset<Remote>(p_free, sizeclass);
      remote.dealloc(target->trunc_id(), offseted, sizeclass);

      stats().remote_post();
      remote.post(&large_allocator, get_trunc_id());
    }

    ChunkMap& chunkmap()
    {
      return chunk_map;
    }
  };
} // namespace snmalloc
