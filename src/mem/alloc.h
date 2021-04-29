#pragma once

#ifdef _MSC_VER
#  define ALLOCATOR __declspec(allocator)
#else
#  define ALLOCATOR
#endif

#include "../pal/pal_consts.h"
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
    FreeListIter small_fast_free_lists[NUM_SMALL_CLASSES];

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
    void* (*InitThreadAllocator)(function_ref<void*(void*)>),
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
    friend RemoteCache;

    LargeAlloc<MemoryProvider> large_allocator;
    ChunkMap chunk_map;
    LocalEntropy entropy;

    /**
     * Per size class bumpptr for building new free lists
     * If aligned to a SLAB start, then it is empty, and a new
     * slab is required.
     */
    CapPtr<void, CBChunk> bump_ptrs[NUM_SMALL_CLASSES] = {nullptr};

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
    template<size_t size, ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc()
    {
      static_assert(size != 0, "Size must not be zero.");
#ifdef SNMALLOC_PASS_THROUGH
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
        return capptr_reveal(small_alloc<zero_mem>(size));
      }
      else if constexpr (sizeclass < NUM_SIZECLASSES)
      {
        handle_message_queue();
        constexpr size_t rsize = sizeclass_to_size(sizeclass);
        return capptr_reveal(medium_alloc<zero_mem>(sizeclass, rsize, size));
      }
      else
      {
        handle_message_queue();
        return capptr_reveal(large_alloc<zero_mem>(size));
      }
#endif
    }

    /**
     * Allocate memory of a dynamically known size.
     */
    template<ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc(size_t size)
    {
#ifdef SNMALLOC_PASS_THROUGH
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
        return capptr_reveal(small_alloc<zero_mem>(size));
      }

      return capptr_reveal(alloc_not_small<zero_mem>(size));
    }

    template<ZeroMem zero_mem = NoZero>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE> alloc_not_small(size_t size)
    {
      handle_message_queue();

      if (size == 0)
      {
        return small_alloc<zero_mem>(1);
      }

      sizeclass_t sizeclass = size_to_sizeclass(size);
      if (sizeclass < NUM_SIZECLASSES)
      {
        size_t rsize = sizeclass_to_size(sizeclass);
        return medium_alloc<zero_mem>(sizeclass, rsize, size);
      }

      return large_alloc<zero_mem>(size);
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
      constexpr sizeclass_t sizeclass = size_to_sizeclass_const(size);

      auto p_ret = CapPtr<void, CBAllocE>(p_raw);
      auto p_auth = large_allocator.capptr_amplify(p_ret);

      if (sizeclass < NUM_SMALL_CLASSES)
      {
        auto super = Superslab::get(p_auth);

        small_dealloc_unchecked(super, p_auth, p_ret, sizeclass);
      }
      else if (sizeclass < NUM_SIZECLASSES)
      {
        auto slab = Mediumslab::get(p_auth);

        medium_dealloc_unchecked(slab, p_auth, p_ret, sizeclass);
      }
      else
      {
        large_dealloc_unchecked(p_auth, p_ret, size);
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

      auto p_ret = CapPtr<void, CBAllocE>(p_raw);
      auto p_auth = large_allocator.capptr_amplify(p_ret);

      if (likely((size - 1) <= (sizeclass_to_size(NUM_SMALL_CLASSES - 1) - 1)))
      {
        auto super = Superslab::get(p_auth);
        sizeclass_t sizeclass = size_to_sizeclass(size);

        small_dealloc_unchecked(super, p_auth, p_ret, sizeclass);
        return;
      }
      dealloc_sized_slow(p_auth, p_ret, size);
#endif
    }

    SNMALLOC_SLOW_PATH void dealloc_sized_slow(
      CapPtr<void, CBArena> p_auth, CapPtr<void, CBAllocE> p_ret, size_t size)
    {
      if (size == 0)
        return dealloc(p_ret.unsafe_capptr, 1);

      if (likely(size <= sizeclass_to_size(NUM_SIZECLASSES - 1)))
      {
        auto slab = Mediumslab::get(p_auth);
        sizeclass_t sizeclass = size_to_sizeclass(size);
        medium_dealloc_unchecked(slab, p_auth, p_ret, sizeclass);
        return;
      }
      large_dealloc_unchecked(p_auth, p_ret, size);
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

      uint8_t chunkmap_slab_kind = chunkmap().get(address_cast(p_raw));

      auto p_ret = CapPtr<void, CBAllocE>(p_raw);
      auto p_auth = large_allocator.capptr_amplify(p_ret);

      if (likely(chunkmap_slab_kind == CMSuperslab))
      {
        /*
         * If this is a live allocation (and not a double- or wild-free), it's
         * safe to construct these Slab and Metaslab pointers and reading the
         * sizeclass won't fail, since either we or the other allocator can't
         * reuse the slab, as we have not yet deallocated this pointer.
         *
         * On the other hand, in the case of a double- or wild-free, this might
         * fault or data race against reused memory.  Eventually, we will come
         * to rely on revocation to guard against these cases: changing the
         * superslab kind will require revoking the whole superslab, as will
         * changing a slab's size class.  However, even then, until we get
         * through the guard in small_dealloc_start(), we must treat this as
         * possibly stale and suspect.
         */
        auto super = Superslab::get(p_auth);
        auto slab = Metaslab::get_slab(p_auth);
        auto meta = super->get_meta(slab);
        sizeclass_t sizeclass = meta->sizeclass();

        small_dealloc_checked_sizeclass(super, slab, p_auth, p_ret, sizeclass);
        return;
      }
      dealloc_not_small(p_auth, p_ret, chunkmap_slab_kind);
    }

    SNMALLOC_SLOW_PATH void dealloc_not_small(
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      uint8_t chunkmap_slab_kind)
    {
      handle_message_queue();

      if (p_ret == nullptr)
        return;

      if (chunkmap_slab_kind == CMMediumslab)
      {
        /*
         * The same reasoning from the fast path continues to hold here.  These
         * values are suspect until we complete the double-free check in
         * medium_dealloc_smart().
         */
        auto slab = Mediumslab::get(p_auth);
        sizeclass_t sizeclass = slab->get_sizeclass();

        medium_dealloc_checked_sizeclass(slab, p_auth, p_ret, sizeclass);
        return;
      }

      if (chunkmap_slab_kind == CMNotOurs)
      {
        error("Not allocated by this allocator");
      }

      large_dealloc_checked_sizeclass(
        p_auth,
        p_ret,
        bits::one_at_bit(chunkmap_slab_kind),
        chunkmap_slab_kind);
#endif
    }

    template<Boundary location = Start>
    void* external_pointer(void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      error("Unsupported");
      UNUSED(p_raw);
#else
      uint8_t chunkmap_slab_kind = chunkmap().get(address_cast(p_raw));
      auto p_ret = CapPtr<void, CBAllocE>(p_raw);
      auto p_auth = large_allocator.capptr_amplify(p_ret);

      auto super = Superslab::get(p_auth);
      if (chunkmap_slab_kind == CMSuperslab)
      {
        auto slab = Metaslab::get_slab(p_auth);
        auto meta = super->get_meta(slab);

        sizeclass_t sc = meta->sizeclass();
        auto slab_end =
          Aal::capptr_rebound(p_ret, pointer_offset(slab, SLAB_SIZE));

        return capptr_reveal(external_pointer<location>(p_ret, sc, slab_end));
      }
      if (chunkmap_slab_kind == CMMediumslab)
      {
        auto slab = Mediumslab::get(p_auth);

        sizeclass_t sc = slab->get_sizeclass();
        auto slab_end =
          Aal::capptr_rebound(p_ret, pointer_offset(slab, SUPERSLAB_SIZE));

        return capptr_reveal(external_pointer<location>(p_ret, sc, slab_end));
      }

      auto ss = super.as_void();

      while (chunkmap_slab_kind >= CMLargeRangeMin)
      {
        // This is a large alloc redirect.
        ss = pointer_offset_signed(
          ss,
          -(static_cast<ptrdiff_t>(1)
            << (chunkmap_slab_kind - CMLargeRangeMin + SUPERSLAB_BITS)));
        chunkmap_slab_kind = chunkmap().get(address_cast(ss));
      }

      if (chunkmap_slab_kind == CMNotOurs)
      {
        if constexpr ((location == End) || (location == OnePastEnd))
          // We don't know the End, so return MAX_PTR
          return pointer_offset<void, void>(nullptr, UINTPTR_MAX);
        else
          // We don't know the Start, so return MIN_PTR
          return nullptr;
      }

      SNMALLOC_ASSERT(
        (chunkmap_slab_kind >= CMLargeMin) &&
        (chunkmap_slab_kind <= CMLargeMax));

      CapPtr<void, CBAllocE> retss = Aal::capptr_rebound(p_ret, ss);
      CapPtr<void, CBAllocE> ret;

      // This is a large alloc, mask off to the slab size.
      if constexpr (location == Start)
        ret = retss;
      else if constexpr (location == End)
        ret = pointer_offset(retss, (bits::one_at_bit(chunkmap_slab_kind)) - 1);
      else
        ret = pointer_offset(retss, bits::one_at_bit(chunkmap_slab_kind));

      return capptr_reveal(ret);
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
      size_t chunkmap_slab_kind = chunkmap().get(address_cast(p_raw));
      auto p_ret = CapPtr<void, CBAllocE>(const_cast<void*>(p_raw));
      auto p_auth = large_allocator.capptr_amplify(p_ret);

      if (likely(chunkmap_slab_kind == CMSuperslab))
      {
        auto super = Superslab::get(p_auth);

        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        auto slab = Metaslab::get_slab(p_auth);
        auto meta = super->get_meta(slab);

        return sizeclass_to_size(meta->sizeclass());
      }

      if (likely(chunkmap_slab_kind == CMMediumslab))
      {
        auto slab = Mediumslab::get(p_auth);
        // Reading a remote sizeclass won't fail, since the other allocator
        // can't reuse the slab, as we have no yet deallocated this pointer.
        return sizeclass_to_size(slab->get_sizeclass());
      }

      if (likely(chunkmap_slab_kind != CMNotOurs))
      {
        SNMALLOC_ASSERT(
          (chunkmap_slab_kind >= CMLargeMin) &&
          (chunkmap_slab_kind <= CMLargeMax));

        return bits::one_at_bit(chunkmap_slab_kind);
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

    SlabList small_classes[NUM_SMALL_CLASSES];
    DLList<Mediumslab, CapPtrCBChunkE> medium_classes[NUM_MEDIUM_CLASSES];

    DLList<Superslab, CapPtrCBChunk> super_available;
    DLList<Superslab, CapPtrCBChunk> super_only_short_available;

    RemoteCache remote_cache;

    std::conditional_t<IsQueueInline, RemoteAllocator, RemoteAllocator*>
      remote_alloc;

#ifdef CACHE_FRIENDLY_OFFSET
    size_t remote_offset = 0;

    template<capptr_bounds B>
    CapPtr<FreeObject, B>
    apply_cache_friendly_offset(CapPtr<void, B> p, sizeclass_t sizeclass)
    {
      size_t mask = sizeclass_to_cache_friendly_mask(sizeclass);

      size_t offset = remote_offset & mask;
      remote_offset += CACHE_FRIENDLY_OFFSET;

      return CapPtr<FreeObject, B>(reinterpret_cast<FreeObject*>(
        reinterpret_cast<uintptr_t>(p.unsafe_capptr) + offset));
    }
#else
    template<capptr_bounds B>
    static CapPtr<FreeObject, B>
    apply_cache_friendly_offset(CapPtr<void, B> p, sizeclass_t sizeclass)
    {
      UNUSED(sizeclass);
      return p.template as_static<FreeObject>();
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

      // Entropy must be first, so that all data-structures can use the key
      // it generates.
      // This must occur before any freelists are constructed.
      entropy.init<typename MemoryProvider::Pal>();

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
        CapPtr<Remote, CBAlloc> p = message_queue().destroy();

        while (p != nullptr)
        {
          auto n = p->non_atomic_next;
          handle_dealloc_remote(p);
          p = n;
        }
      }

      // Dump bump allocators back into memory
      for (size_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        auto& bp = bump_ptrs[i];
        auto rsize = sizeclass_to_size(i);
        FreeListIter ffl;

        CapPtr<Superslab, CBChunk> super = Superslab::get(bp);
        auto super_slabd = capptr_debug_chunkd_from_chunk(super);

        CapPtr<Slab, CBChunk> slab = Metaslab::get_slab(bp);
        auto slab_slabd = capptr_debug_chunkd_from_chunk(slab);

        while (pointer_align_up(bp, SLAB_SIZE) != bp)
        {
          Slab::alloc_new_list(bp, ffl, rsize, entropy);
          while (!ffl.empty())
          {
            small_dealloc_offseted_inner(
              super_slabd, slab_slabd, ffl.take(entropy), i);
          }
        }
      }

      for (size_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        if (!small_fast_free_lists[i].empty())
        {
          auto head = small_fast_free_lists[i].peek();
          auto head_auth = large_allocator.capptr_amplify(head);
          auto super = Superslab::get(head_auth);
          auto slab = Metaslab::get_slab(head_auth);
          do
          {
            auto curr = small_fast_free_lists[i].take(entropy);
            small_dealloc_offseted_inner(super, slab, curr, i);
          } while (!small_fast_free_lists[i].empty());

          test(small_classes[i]);
        }
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
    static CapPtr<void, CBAllocE> external_pointer(
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass,
      CapPtr<void, CBAllocE> end_point)
    {
      size_t rsize = sizeclass_to_size(sizeclass);

      auto end_point_correction = location == End ?
        pointer_offset_signed(end_point, -1) :
        (location == OnePastEnd ?
           end_point :
           pointer_offset_signed(end_point, -static_cast<ptrdiff_t>(rsize)));

      size_t offset_from_end =
        pointer_diff(p_ret, pointer_offset_signed(end_point, -1));

      size_t end_to_end = round_by_sizeclass(sizeclass, offset_from_end);

      return pointer_offset_signed(
        end_point_correction, -static_cast<ptrdiff_t>(end_to_end));
    }

    void init_message_queue()
    {
      // Manufacture an allocation to prime the queue
      // Using an actual allocation removes a conditional from a critical path.
      auto dummy = CapPtr<void, CBAlloc>(alloc<YesZero>(MIN_ALLOC_SIZE))
                     .template as_static<Remote>();
      if (dummy == nullptr)
      {
        error("Critical error: Out-of-memory during initialisation.");
      }
      dummy->set_info(get_trunc_id(), size_to_sizeclass_const(MIN_ALLOC_SIZE));
      message_queue().init(dummy);
    }

    SNMALLOC_FAST_PATH void handle_dealloc_remote(CapPtr<Remote, CBAlloc> p)
    {
      auto target_id = Remote::trunc_target_id(p, &large_allocator);
      if (likely(target_id == get_trunc_id()))
      {
        // Destined for my slabs
        auto p_auth = large_allocator.template capptr_amplify<Remote>(p);
        auto super = Superslab::get(p_auth);
        dealloc_not_large_local(super, p, p->sizeclass());
      }
      else
      {
        // Merely routing; despite the cast here, p is going to be cast right
        // back to a Remote.
        remote_cache.dealloc<Allocator>(
          target_id, p.template as_reinterpret<FreeObject>(), p->sizeclass());
      }
    }

    SNMALLOC_SLOW_PATH void dealloc_not_large(
      RemoteAllocator* target, CapPtr<void, CBAlloc> p, sizeclass_t sizeclass)
    {
      if (likely(target->trunc_id() == get_trunc_id()))
      {
        auto p_auth = large_allocator.capptr_amplify(p);
        auto super = Superslab::get(p_auth);
        auto offseted = apply_cache_friendly_offset(p, sizeclass)
                          .template as_reinterpret<Remote>();
        dealloc_not_large_local(super, offseted, sizeclass);
      }
      else
      {
        remote_dealloc_and_post(target, p, sizeclass);
      }
    }

    SNMALLOC_FAST_PATH void dealloc_not_large_local(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Remote, CBAlloc> p_offseted,
      sizeclass_t sizeclass)
    {
      // Guard against remote queues that have colliding IDs
      SNMALLOC_ASSERT(super->get_allocator() == public_state());

      if (likely(sizeclass < NUM_SMALL_CLASSES))
      {
        SNMALLOC_ASSERT(super->get_kind() == Super);
        check_client(
          super->get_kind() == Super,
          "Heap Corruption: Sizeclass of remote dealloc corrupt.");
        auto slab =
          Metaslab::get_slab(Aal::capptr_rebound(super.as_void(), p_offseted));
        check_client(
          super->get_meta(slab)->sizeclass() == sizeclass,
          "Heap Corruption: Sizeclass of remote dealloc corrupt.");
        small_dealloc_offseted(
          super, slab, FreeObject::make(p_offseted), sizeclass);
      }
      else
      {
        auto medium = super.template as_reinterpret<Mediumslab>();
        SNMALLOC_ASSERT(medium->get_kind() == Medium);
        check_client(
          medium->get_kind() == Medium,
          "Heap Corruption: Sizeclass of remote dealloc corrupt.");
        check_client(
          medium->get_sizeclass() == sizeclass,
          "Heap Corruption: Sizeclass of remote dealloc corrupt.");
        medium_dealloc_local(
          medium, Remote::clear(p_offseted, sizeclass), sizeclass);
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
      if (likely(remote_cache.capacity > 0))
        return;

      stats().remote_post();
      remote_cache.post<Allocator>(this, get_trunc_id());
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

    CapPtr<Superslab, CBChunk> get_superslab()
    {
      auto super = super_available.get_head();

      if (super != nullptr)
        return super;

      super = large_allocator
                .template alloc<NoZero>(0, SUPERSLAB_SIZE, SUPERSLAB_SIZE)
                .template as_reinterpret<Superslab>();

      if (super == nullptr)
        return super;

      super->init(public_state());
      chunkmap().set_slab(super);
      super_available.insert(super);
      return super;
    }

    void reposition_superslab(CapPtr<Superslab, CBChunk> super)
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

    SNMALLOC_SLOW_PATH CapPtr<Slab, CBChunk> alloc_slab(sizeclass_t sizeclass)
    {
      stats().sizeclass_alloc_slab(sizeclass);
      if (Superslab::is_short_sizeclass(sizeclass))
      {
        // Pull a short slab from the list of superslabs that have only the
        // short slab available.
        CapPtr<Superslab, CBChunk> super = super_only_short_available.pop();

        if (super != nullptr)
        {
          auto slab = Superslab::alloc_short_slab(super, sizeclass);
          SNMALLOC_ASSERT(super->is_full());
          return slab;
        }

        super = get_superslab();

        if (super == nullptr)
          return nullptr;

        auto slab = Superslab::alloc_short_slab(super, sizeclass);
        reposition_superslab(super);
        return slab;
      }

      auto super = get_superslab();

      if (super == nullptr)
        return nullptr;

      auto slab = Superslab::alloc_slab(super, sizeclass);
      reposition_superslab(super);
      return slab;
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE> small_alloc(size_t size)
    {
      SNMALLOC_ASSUME(size <= SLAB_SIZE);
      sizeclass_t sizeclass = size_to_sizeclass(size);
      return small_alloc_inner<zero_mem>(sizeclass, size);
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE>
    small_alloc_inner(sizeclass_t sizeclass, size_t size)
    {
      SNMALLOC_ASSUME(sizeclass < NUM_SMALL_CLASSES);
      auto& fl = small_fast_free_lists[sizeclass];
      if (likely(!fl.empty()))
      {
        stats().alloc_request(size);
        stats().sizeclass_alloc(sizeclass);
        auto p = remove_cache_friendly_offset(fl.take(entropy), sizeclass);
        if constexpr (zero_mem == YesZero)
        {
          pal_zero<typename MemoryProvider::Pal>(
            p, sizeclass_to_size(sizeclass));
        }

        return capptr_export(p);
      }

      if (likely(!has_messages()))
        return small_alloc_next_free_list<zero_mem>(sizeclass, size);

      return small_alloc_mq_slow<zero_mem>(sizeclass, size);
    }

    /**
     * Slow path for handling message queue, before dealing with small
     * allocation request.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_mq_slow(sizeclass_t sizeclass, size_t size)
    {
      handle_message_queue_inner();

      return small_alloc_next_free_list<zero_mem>(sizeclass, size);
    }

    /**
     * Attempt to find a new free list to allocate from
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_next_free_list(sizeclass_t sizeclass, size_t size)
    {
      size_t rsize = sizeclass_to_size(sizeclass);
      auto& sl = small_classes[sizeclass];

      if (likely(!sl.is_empty()))
      {
        stats().alloc_request(size);
        stats().sizeclass_alloc(sizeclass);

        auto meta = sl.get_next().template as_static<Metaslab>();
        auto& ffl = small_fast_free_lists[sizeclass];
        return Metaslab::alloc<zero_mem, typename MemoryProvider::Pal>(
          meta, ffl, rsize, entropy);
      }
      return small_alloc_rare<zero_mem>(sizeclass, size);
    }

    /**
     * Called when there are no available free list to service this request
     * Could be due to using the dummy allocator, or needing to bump allocate a
     * new free list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_rare(sizeclass_t sizeclass, size_t size)
    {
      if (likely(!NeedsInitialisation(this)))
      {
        stats().alloc_request(size);
        stats().sizeclass_alloc(sizeclass);
        return small_alloc_new_free_list<zero_mem>(sizeclass);
      }
      return small_alloc_first_alloc<zero_mem>(sizeclass, size);
    }

    /**
     * Called on first allocation to set up the thread local allocator,
     * then directs the allocation request to the newly created allocator.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_first_alloc(sizeclass_t sizeclass, size_t size)
    {
      /*
       * We have to convert through void* as part of the thread allocator
       * initializer API.  Be a little more verbose than strictly necessary to
       * demonstrate that small_alloc_inner is giving us a CBAllocE-annotated
       * pointer before we just go slapping that label on a void* later.
       */
      void* ret = InitThreadAllocator([sizeclass, size](void* alloc) {
        CapPtr<void, CBAllocE> ret =
          reinterpret_cast<Allocator*>(alloc)
            ->template small_alloc_inner<zero_mem>(sizeclass, size);
        return ret.unsafe_capptr;
      });
      return CapPtr<void, CBAllocE>(ret);
    }

    /**
     * Called to create a new free list, and service the request from that new
     * list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE>
    small_alloc_new_free_list(sizeclass_t sizeclass)
    {
      auto& bp = bump_ptrs[sizeclass];
      if (likely(pointer_align_up(bp, SLAB_SIZE) != bp))
      {
        return small_alloc_build_free_list<zero_mem>(sizeclass);
      }
      // Fetch new slab
      return small_alloc_new_slab<zero_mem>(sizeclass);
    }

    /**
     * Creates a new free list from the thread local bump allocator and service
     * the request from that new list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE>
    small_alloc_build_free_list(sizeclass_t sizeclass)
    {
      auto& bp = bump_ptrs[sizeclass];
      auto rsize = sizeclass_to_size(sizeclass);
      auto& ffl = small_fast_free_lists[sizeclass];
      SNMALLOC_ASSERT(ffl.empty());
      Slab::alloc_new_list(bp, ffl, rsize, entropy);

      auto p = remove_cache_friendly_offset(ffl.take(entropy), sizeclass);

      if constexpr (zero_mem == YesZero)
      {
        pal_zero<typename MemoryProvider::Pal>(p, sizeclass_to_size(sizeclass));
      }

      return capptr_export(p);
    }

    /**
     * Allocates a new slab to allocate from, set it to be the bump allocator
     * for this size class, and then builds a new free list from the thread
     * local bump allocator and service the request from that new list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_new_slab(sizeclass_t sizeclass)
    {
      auto& bp = bump_ptrs[sizeclass];
      // Fetch new slab
      auto slab = alloc_slab(sizeclass);
      if (slab == nullptr)
        return nullptr;
      bp = pointer_offset(
        slab, get_initial_offset(sizeclass, Metaslab::is_short(slab)));

      return small_alloc_build_free_list<zero_mem>(sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_unchecked(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      check_client(
        chunkmap().get(address_cast(p_ret)) == CMSuperslab,
        "Claimed small deallocation is not in a Superslab");

      small_dealloc_checked_chunkmap(super, p_auth, p_ret, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_checked_chunkmap(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      auto slab = Metaslab::get_slab(p_auth);
      check_client(
        sizeclass == super->get_meta(slab)->sizeclass(),
        "Claimed small deallocation with mismatching size class");

      small_dealloc_checked_sizeclass(super, slab, p_auth, p_ret, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_checked_sizeclass(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Slab, CBChunkD> slab,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      check_client(
        Metaslab::is_start_of_object(Slab::get_meta(slab), address_cast(p_ret)),
        "Not deallocating start of an object");

      small_dealloc_start(super, slab, p_auth, p_ret, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_start(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Slab, CBChunkD> slab,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      // TODO: with SSM/MTE, guard against double-frees
      UNUSED(p_ret);

      RemoteAllocator* target = super->get_allocator();

      auto p =
        Aal::capptr_bound<void, CBAlloc>(p_auth, sizeclass_to_size(sizeclass));

      if (likely(target == public_state()))
      {
        auto offseted = apply_cache_friendly_offset(p, sizeclass);
        small_dealloc_offseted(super, slab, offseted, sizeclass);
      }
      else
        remote_dealloc(target, p, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_offseted(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Slab, CBChunkD> slab,
      CapPtr<FreeObject, CBAlloc> p,
      sizeclass_t sizeclass)
    {
      stats().sizeclass_dealloc(sizeclass);

      small_dealloc_offseted_inner(super, slab, p, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_offseted_inner(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Slab, CBChunkD> slab,
      CapPtr<FreeObject, CBAlloc> p,
      sizeclass_t sizeclass)
    {
      if (likely(Slab::dealloc_fast(slab, super, p, entropy)))
        return;

      small_dealloc_offseted_slow(super, slab, p, sizeclass);
    }

    SNMALLOC_SLOW_PATH void small_dealloc_offseted_slow(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Slab, CBChunkD> slab,
      CapPtr<FreeObject, CBAlloc> p,
      sizeclass_t sizeclass)
    {
      bool was_full = super->is_full();
      SlabList* sl = &small_classes[sizeclass];
      Superslab::Action a = Slab::dealloc_slow(slab, sl, super, p, entropy);
      if (likely(a == Superslab::NoSlabReturn))
        return;
      stats().sizeclass_dealloc_slab(sizeclass);

      if (a == Superslab::NoStatusChange)
        return;

      auto super_slab = capptr_chunk_from_chunkd(super, SUPERSLAB_SIZE);

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
            super_available.insert(super_slab);
          }
          else
          {
            super_only_short_available.remove(super_slab);
            super_available.insert(super_slab);
          }
          break;
        }

        case Superslab::OnlyShortSlabAvailable:
        {
          super_only_short_available.insert(super_slab);
          break;
        }

        case Superslab::Empty:
        {
          super_available.remove(super_slab);

          chunkmap().clear_slab(super_slab);
          large_allocator.dealloc(
            super_slab.template as_reinterpret<Largeslab>(), 0);
          stats().superslab_push();
          break;
        }
      }
    }

    template<ZeroMem zero_mem>
    CapPtr<void, CBAllocE>
    medium_alloc(sizeclass_t sizeclass, size_t rsize, size_t size)
    {
      sizeclass_t medium_class = sizeclass - NUM_SMALL_CLASSES;

      auto sc = &medium_classes[medium_class];
      CapPtr<Mediumslab, CBChunkE> slab = sc->get_head();
      CapPtr<void, CBAllocE> p;

      if (slab != nullptr)
      {
        p = Mediumslab::alloc<zero_mem, typename MemoryProvider::Pal>(
          slab, rsize);

        if (Mediumslab::full(slab))
          sc->pop();
      }
      else
      {
        if (NeedsInitialisation(this))
        {
          /*
           * We have to convert through void* as part of the thread allocator
           * initializer API.  Be a little more verbose than strictly necessary
           * to demonstrate that small_alloc_inner is giving us an annotated
           * pointer before we just go slapping that label on a void* later.
           */
          void* ret =
            InitThreadAllocator([size, rsize, sizeclass](void* alloc) {
              CapPtr<void, CBAllocE> ret =
                reinterpret_cast<Allocator*>(alloc)->medium_alloc<zero_mem>(
                  sizeclass, rsize, size);
              return ret.unsafe_capptr;
            });
          return CapPtr<void, CBAllocE>(ret);
        }

        auto newslab =
          large_allocator
            .template alloc<NoZero>(0, SUPERSLAB_SIZE, SUPERSLAB_SIZE)
            .template as_reinterpret<Mediumslab>();

        if (newslab == nullptr)
          return nullptr;

        Mediumslab::init(newslab, public_state(), sizeclass, rsize);
        chunkmap().set_slab(newslab);

        auto newslab_export = capptr_export(newslab);

        p = Mediumslab::alloc<zero_mem, typename MemoryProvider::Pal>(
          newslab_export, rsize);

        if (!Mediumslab::full(newslab))
          sc->insert(newslab_export);
      }

      stats().alloc_request(size);
      stats().sizeclass_alloc(sizeclass);

      return p;
    }

    SNMALLOC_FAST_PATH
    void medium_dealloc_unchecked(
      CapPtr<Mediumslab, CBChunkD> slab,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      check_client(
        chunkmap().get(address_cast(p_ret)) == CMMediumslab,
        "Claimed medium deallocation is not in a Mediumslab");

      medium_dealloc_checked_chunkmap(slab, p_auth, p_ret, sizeclass);
    }

    SNMALLOC_FAST_PATH
    void medium_dealloc_checked_chunkmap(
      CapPtr<Mediumslab, CBChunkD> slab,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      check_client(
        slab->get_sizeclass() == sizeclass,
        "Claimed medium deallocation of the wrong sizeclass");

      medium_dealloc_checked_sizeclass(slab, p_auth, p_ret, sizeclass);
    }

    SNMALLOC_FAST_PATH
    void medium_dealloc_checked_sizeclass(
      CapPtr<Mediumslab, CBChunkD> slab,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      check_client(
        is_multiple_of_sizeclass(
          sizeclass, address_cast(slab) + SUPERSLAB_SIZE - address_cast(p_ret)),
        "Not deallocating start of an object");

      medium_dealloc_start(slab, p_auth, p_ret, sizeclass);
    }

    SNMALLOC_FAST_PATH
    void medium_dealloc_start(
      CapPtr<Mediumslab, CBChunkD> slab,
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      sizeclass_t sizeclass)
    {
      // TODO: with SSM/MTE, guard against double-frees
      UNUSED(p_ret);

      RemoteAllocator* target = slab->get_allocator();

      // TODO: This bound is perhaps superfluous in the local case, as
      // mediumslabs store free objects by offset rather than pointer.
      auto p =
        Aal::capptr_bound<void, CBAlloc>(p_auth, sizeclass_to_size(sizeclass));

      if (likely(target == public_state()))
        medium_dealloc_local(slab, p, sizeclass);
      else
      {
        remote_dealloc(target, p, sizeclass);
      }
    }

    SNMALLOC_FAST_PATH
    void medium_dealloc_local(
      CapPtr<Mediumslab, CBChunkD> slab,
      CapPtr<void, CBAlloc> p,
      sizeclass_t sizeclass)
    {
      stats().sizeclass_dealloc(sizeclass);
      bool was_full = Mediumslab::dealloc(slab, p);

      auto slab_bounded = capptr_chunk_from_chunkd(slab, SUPERSLAB_SIZE);

      if (Mediumslab::empty(slab))
      {
        if (!was_full)
        {
          sizeclass_t medium_class = sizeclass - NUM_SMALL_CLASSES;
          auto sc = &medium_classes[medium_class];
          /*
           * This unsafety lets us avoid applying platform constraints to a
           * pointer we are just about to drop on the floor; remove() uses its
           * argument but does not persist it.
           */
          sc->remove(CapPtr<Mediumslab, CBChunkE>(slab_bounded.unsafe_capptr));
        }

        chunkmap().clear_slab(slab_bounded);
        large_allocator.dealloc(
          slab_bounded.template as_reinterpret<Largeslab>(), 0);
        stats().superslab_push();
      }
      else if (was_full)
      {
        sizeclass_t medium_class = sizeclass - NUM_SMALL_CLASSES;
        auto sc = &medium_classes[medium_class];
        sc->insert(capptr_export(slab_bounded));
      }
    }

    template<ZeroMem zero_mem>
    CapPtr<void, CBAllocE> large_alloc(size_t size)
    {
      if (NeedsInitialisation(this))
      {
        // MSVC-vs-CapPtr triggering; xref CapPtr's constructor
        void* ret = InitThreadAllocator([size](void* alloc) {
          CapPtr<void, CBAllocE> ret =
            reinterpret_cast<Allocator*>(alloc)->large_alloc<zero_mem>(size);
          return ret.unsafe_capptr;
        });
        return CapPtr<void, CBAllocE>(ret);
      }

      size_t size_bits = bits::next_pow2_bits(size);
      size_t large_class = size_bits - SUPERSLAB_BITS;
      SNMALLOC_ASSERT(large_class < NUM_LARGE_CLASSES);

      size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      // For superslab size, we always commit the whole range.
      if (large_class == 0)
        size = rsize;

      CapPtr<Largeslab, CBChunk> p =
        large_allocator.template alloc<zero_mem>(large_class, rsize, size);
      if (likely(p != nullptr))
      {
        chunkmap().set_large_size(p, size);

        stats().alloc_request(size);
        stats().large_alloc(large_class);
      }
      return capptr_export(Aal::capptr_bound<void, CBAlloc>(p, rsize));
    }

    void large_dealloc_unchecked(
      CapPtr<void, CBArena> p_auth, CapPtr<void, CBAllocE> p_ret, size_t size)
    {
      uint8_t claimed_chunkmap_slab_kind =
        static_cast<uint8_t>(bits::next_pow2_bits(size));

      // This also catches some "not deallocating start of an object" cases: if
      // we're so far from the start that our actual chunkmap slab kind is not a
      // legitimate large class
      check_client(
        chunkmap().get(address_cast(p_ret)) == claimed_chunkmap_slab_kind,
        "Claimed large deallocation with wrong size class");

      // round up as we would if we had had to look up the chunkmap_slab_kind
      size_t rsize = bits::one_at_bit(claimed_chunkmap_slab_kind);

      large_dealloc_checked_sizeclass(
        p_auth, p_ret, rsize, claimed_chunkmap_slab_kind);
    }

    void large_dealloc_checked_sizeclass(
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      size_t size,
      uint8_t chunkmap_slab_kind)
    {
      check_client(
        address_cast(Superslab::get(p_auth)) == address_cast(p_ret),
        "Not deallocating start of an object");
      SNMALLOC_ASSERT(bits::one_at_bit(chunkmap_slab_kind) >= SUPERSLAB_SIZE);

      large_dealloc_start(p_auth, p_ret, size, chunkmap_slab_kind);
    }

    void large_dealloc_start(
      CapPtr<void, CBArena> p_auth,
      CapPtr<void, CBAllocE> p_ret,
      size_t size,
      uint8_t chunkmap_slab_kind)
    {
      // TODO: with SSM/MTE, guard against double-frees

      if (NeedsInitialisation(this))
      {
        InitThreadAllocator(
          [p_auth, p_ret, size, chunkmap_slab_kind](void* alloc) {
            reinterpret_cast<Allocator*>(alloc)->large_dealloc_start(
              p_auth, p_ret, size, chunkmap_slab_kind);
            return nullptr;
          });
        return;
      }

      size_t large_class = chunkmap_slab_kind - SUPERSLAB_BITS;
      auto slab = Aal::capptr_bound<Largeslab, CBChunk>(p_auth, size);

      chunkmap().clear_large_size(slab, size);

      stats().large_dealloc(large_class);

      // Initialise in order to set the correct SlabKind.
      slab->init();
      large_allocator.dealloc(slab, large_class);
    }

    // This is still considered the fast path as all the complex code is tail
    // called in its slow path. This leads to one fewer unconditional jump in
    // Clang.
    SNMALLOC_FAST_PATH
    void remote_dealloc(
      RemoteAllocator* target, CapPtr<void, CBAlloc> p, sizeclass_t sizeclass)
    {
      SNMALLOC_ASSERT(target->trunc_id() != get_trunc_id());

      // Check whether this will overflow the cache first.  If we are a fake
      // allocator, then our cache will always be full and so we will never hit
      // this path.
      if (remote_cache.capacity > 0)
      {
        stats().remote_free(sizeclass);
        auto offseted = apply_cache_friendly_offset(p, sizeclass);
        remote_cache.dealloc<Allocator>(
          target->trunc_id(), offseted, sizeclass);
        return;
      }

      remote_dealloc_slow(target, p, sizeclass);
    }

    SNMALLOC_SLOW_PATH void remote_dealloc_slow(
      RemoteAllocator* target,
      CapPtr<void, CBAlloc> p_auth,
      sizeclass_t sizeclass)
    {
      SNMALLOC_ASSERT(target->trunc_id() != get_trunc_id());

      // Now that we've established that we're in the slow path (if we're a
      // real allocator, we will have to empty our cache now), check if we are
      // a real allocator and construct one if we aren't.
      if (NeedsInitialisation(this))
      {
        InitThreadAllocator([target, p_auth, sizeclass](void* alloc) {
          reinterpret_cast<Allocator*>(alloc)->dealloc_not_large(
            target, p_auth, sizeclass);
          return nullptr;
        });
        return;
      }

      remote_dealloc_and_post(target, p_auth, sizeclass);
    }

    SNMALLOC_SLOW_PATH void remote_dealloc_and_post(
      RemoteAllocator* target,
      CapPtr<void, CBAlloc> p_auth,
      sizeclass_t sizeclass)
    {
      handle_message_queue();

      stats().remote_free(sizeclass);
      auto offseted = apply_cache_friendly_offset(p_auth, sizeclass);
      remote_cache.dealloc<Allocator>(target->trunc_id(), offseted, sizeclass);

      stats().remote_post();
      remote_cache.post<Allocator>(this, get_trunc_id());
    }

    ChunkMap& chunkmap()
    {
      return chunk_map;
    }
  };
} // namespace snmalloc
