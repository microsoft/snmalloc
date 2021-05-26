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
#include "fastcache.h"
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

  /**
   * Allocator.  This class is parameterised on three template parameters.
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
    class MemoryProvider = GlobalVirtual,
    class ChunkMap = SNMALLOC_DEFAULT_CHUNKMAP,
    bool IsQueueInline = true>
  class Allocator
  : public Pooled<Allocator<MemoryProvider, ChunkMap, IsQueueInline>>
  {
    friend RemoteCache;

    template<typename Alloc, void (*register_clean_up)()>
    friend class FastAllocator;

    LargeAlloc<MemoryProvider> large_allocator;
    ChunkMap chunk_map;
    LocalEntropy entropy;
    FastCache* attached_cache;

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

    Stats* attached_stats()
    {
      if (attached_cache == nullptr)
        return nullptr;
      return &(attached_cache->stats);
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
      stats().sizeclass_alloc(sizeclass);

      if constexpr (sizeclass < NUM_SMALL_CLASSES)
      {
        return capptr_reveal(small_alloc_one<zero_mem>(size));
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
        return small_alloc_one<zero_mem>(size);
        // return capptr_reveal(small_alloc<zero_mem>(size));
      }

      return alloc_not_small<zero_mem>(size);
#endif
    }

    template<ZeroMem zero_mem = NoZero>
    void* small_alloc_one(size_t size)
    {
      if (attached_cache != nullptr)
        return attached_cache->template alloc<zero_mem>(
          size, [&](sizeclass_t sizeclass, FreeListIter* fl) {
            return small_alloc<zero_mem>(sizeclass, *fl);
          });

      auto sizeclass = size_to_sizeclass(size);
      stats().alloc_request(size);
      stats().sizeclass_alloc(sizeclass);

      // This is a debug path.  When we reallocate a message queue in
      // debug check empty, that might occur when the allocator is not attached
      // to any thread.  Hence, the following unperformant code is acceptable.
      // TODO: Potentially do something nicer.
      FreeListIter temp;
      auto r = small_alloc<zero_mem>(sizeclass, temp);
      while (!temp.empty())
      {
        // Fake statistics up.
        stats().sizeclass_alloc(sizeclass);
        dealloc(capptr_reveal(capptr_export(temp.take(entropy).as_void())));
      }
      return r;
    }

    template<ZeroMem zero_mem = NoZero>
    SNMALLOC_SLOW_PATH void* alloc_not_small(size_t size)
    {
      handle_message_queue();

      if (size == 0)
      {
        return small_alloc_one<zero_mem>(1);
      }

      sizeclass_t sizeclass = size_to_sizeclass(size);
      if (sizeclass < NUM_SIZECLASSES)
      {
        size_t rsize = sizeclass_to_size(sizeclass);
        return capptr_reveal(medium_alloc<zero_mem>(sizeclass, rsize, size));
      }

      return capptr_reveal(large_alloc<zero_mem>(size));
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
      FastCache* cache,
      MemoryProvider& m,
      ChunkMap&& c = ChunkMap(),
      RemoteAllocator* r = nullptr,
      bool isFake = false)
    : large_allocator(m), chunk_map(c), attached_cache(cache)
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
      stats().start();

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

      if (attached_cache != nullptr)
        attached_cache->flush([&](auto p) { dealloc(p); });

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
        auto sizeclass = p->sizeclass();
        dealloc_not_large_local(super, Remote::clear(p), sizeclass);
      }
      else
      {
        // Merely routing; despite the cast here, p is going to be cast right
        // back to a Remote.
        remote_cache.dealloc<Allocator>(
          target_id, p.template as_reinterpret<void>(), p->sizeclass());
      }
    }

    SNMALLOC_SLOW_PATH void dealloc_not_large(
      RemoteAllocator* target, CapPtr<void, CBAlloc> p, sizeclass_t sizeclass)
    {
      if (likely(target->trunc_id() == get_trunc_id()))
      {
        auto p_auth = large_allocator.capptr_amplify(p);
        auto super = Superslab::get(p_auth);
        dealloc_not_large_local(super, p, sizeclass);
      }
      else
      {
        remote_dealloc_and_post(target, p, sizeclass);
      }
    }

    // TODO: Adjust when medium slab same as super slab.
    //    Second parameter should be a FreeObject.
    SNMALLOC_FAST_PATH void dealloc_not_large_local(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<void, CBAlloc> p,
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
        auto slab = Metaslab::get_slab(Aal::capptr_rebound(super.as_void(), p));
        check_client(
          super->get_meta(slab)->sizeclass() == sizeclass,
          "Heap Corruption: Sizeclass of remote dealloc corrupt.");
        small_dealloc_offseted(super, slab, p, sizeclass);
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
        medium_dealloc_local(medium, p, sizeclass);
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
    SNMALLOC_SLOW_PATH void*
    small_alloc(sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      if (likely(!has_messages()))
        return capptr_reveal(
          small_alloc_next_free_list<zero_mem>(sizeclass, fast_free_list));

      return capptr_reveal(
        small_alloc_mq_slow<zero_mem>(sizeclass, fast_free_list));
    }

    /**
     * Slow path for handling message queue, before dealing with small
     * allocation request.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_mq_slow(sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      handle_message_queue_inner();

      return small_alloc_next_free_list<zero_mem>(sizeclass, fast_free_list);
    }

    /**
     * Attempt to find a new free list to allocate from
     */
    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE> small_alloc_next_free_list(
      sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      size_t rsize = sizeclass_to_size(sizeclass);
      auto& sl = small_classes[sizeclass];

      if (likely(!sl.is_empty()))
      {
        auto meta = sl.get_next().template as_static<Metaslab>();
        return Metaslab::alloc<zero_mem, typename MemoryProvider::Pal>(
          meta, fast_free_list, rsize, entropy);
      }
      return small_alloc_rare<zero_mem>(sizeclass, fast_free_list);
    }

    /**
     * Called when there are no available free list to service this request
     * Could be due to using the dummy allocator, or needing to bump allocate a
     * new free list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_rare(sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      return small_alloc_new_free_list<zero_mem>(sizeclass, fast_free_list);
    }

    /**
     * Called to create a new free list, and service the request from that new
     * list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE> small_alloc_new_free_list(
      sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      auto& bp = bump_ptrs[sizeclass];
      if (likely(pointer_align_up(bp, SLAB_SIZE) != bp))
      {
        return small_alloc_build_free_list<zero_mem>(sizeclass, fast_free_list);
      }
      // Fetch new slab
      return small_alloc_new_slab<zero_mem>(sizeclass, fast_free_list);
    }

    /**
     * Creates a new free list from the thread local bump allocator and service
     * the request from that new list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH CapPtr<void, CBAllocE> small_alloc_build_free_list(
      sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      auto& bp = bump_ptrs[sizeclass];
      auto rsize = sizeclass_to_size(sizeclass);
      SNMALLOC_ASSERT(fast_free_list.empty());
      Slab::alloc_new_list(bp, fast_free_list, rsize, entropy);

      auto p = fast_free_list.take(entropy);

      if constexpr (zero_mem == YesZero)
      {
        pal_zero<typename MemoryProvider::Pal>(p, sizeclass_to_size(sizeclass));
      }

      // TODO: Should this be zeroing the next pointer?
      return capptr_export(p.as_void());
    }

    /**
     * Allocates a new slab to allocate from, set it to be the bump allocator
     * for this size class, and then builds a new free list from the thread
     * local bump allocator and service the request from that new list.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH CapPtr<void, CBAllocE>
    small_alloc_new_slab(sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      auto& bp = bump_ptrs[sizeclass];
      // Fetch new slab
      auto slab = alloc_slab(sizeclass);
      if (slab == nullptr)
        return nullptr;
      bp = pointer_offset(
        slab, get_initial_offset(sizeclass, Metaslab::is_short(slab)));

      return small_alloc_build_free_list<zero_mem>(sizeclass, fast_free_list);
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
        Slab::get_meta(slab)->is_start_of_object(address_cast(p_ret)),
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
        small_dealloc_offseted(super, slab, p, sizeclass);
      }
      else
        remote_dealloc(target, p, sizeclass);
    }

    SNMALLOC_FAST_PATH void small_dealloc_offseted(
      CapPtr<Superslab, CBChunkD> super,
      CapPtr<Slab, CBChunkD> slab,
      CapPtr<void, CBAlloc> p,
      sizeclass_t sizeclass)
    {
      stats().sizeclass_dealloc(sizeclass);

      small_dealloc_offseted_inner(super, slab, FreeObject::make(p), sizeclass);
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
      size_t size_bits = bits::next_pow2_bits(size);
      size_t large_class = size_bits - SUPERSLAB_BITS;
      SNMALLOC_ASSERT(large_class < NUM_LARGE_CLASSES);

      stats().alloc_request(size);
      stats().large_alloc(large_class);

      size_t rsize = bits::one_at_bit(SUPERSLAB_BITS) << large_class;
      // For superslab size, we always commit the whole range.
      if (large_class == 0)
        size = rsize;

      CapPtr<Largeslab, CBChunk> p =
        large_allocator.template alloc<zero_mem>(large_class, rsize, size);
      if (likely(p != nullptr))
      {
        chunkmap().set_large_size(p, size);
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
      UNUSED(p_ret);
      SNMALLOC_ASSERT(bits::one_at_bit(chunkmap_slab_kind) >= SUPERSLAB_SIZE);

      large_dealloc_start(p_auth, size, chunkmap_slab_kind);
    }

    void large_dealloc_start(
      CapPtr<void, CBArena> p_auth, size_t size, uint8_t chunkmap_slab_kind)
    {
      // TODO: with SSM/MTE, guard against double-frees

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
        remote_cache.dealloc<Allocator>(target->trunc_id(), p, sizeclass);
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

      remote_dealloc_and_post(target, p_auth, sizeclass);
    }

    SNMALLOC_SLOW_PATH void remote_dealloc_and_post(
      RemoteAllocator* target,
      CapPtr<void, CBAlloc> p_auth,
      sizeclass_t sizeclass)
    {
      handle_message_queue();

      stats().remote_free(sizeclass);
      remote_cache.dealloc<Allocator>(target->trunc_id(), p_auth, sizeclass);

      stats().remote_post();
      remote_cache.post<Allocator>(this, get_trunc_id());
    }

    ChunkMap& chunkmap()
    {
      return chunk_map;
    }
  };
} // namespace snmalloc
