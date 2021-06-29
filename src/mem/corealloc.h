#pragma once
#include "../ds/defines.h"
#include "../backend/slaballocator.h"
#include "allocconfig.h"
#include "fastcache.h"
#include "metaslab.h"
#include "pooled.h"
#include "remotecache.h"
#include "sizeclasstable.h"

namespace snmalloc
{
  template<typename SharedStateHandle>
  class CoreAlloc : public Pooled<CoreAlloc<SharedStateHandle>>
  {
    template<class SharedStateHandle2>
    friend class FastAllocator;

    /**
     * Per size class list of active slabs for this allocator.
     */
    SlabList alloc_classes[NUM_SIZECLASSES];

    /**
     * Remote deallocations for other threads
     */
    RemoteCache remote_cache;

    /**
     * Local entropy source and current version of keys for
     * this thread
     */
    LocalEntropy entropy;

    /**
     * Message queue for allocations being returned to this
     * allocator
     */
    std::conditional_t<
      SharedStateHandle::IsQueueInline,
      RemoteAllocator,
      RemoteAllocator*>
      remote_alloc;

    /**
     * A local area of address space managed by this allocator.
     * Used to reduce calls on the global address space.
     */
    AddressSpaceManagerCore<typename SharedStateHandle::Pal>
      local_address_space;

    /**
     * This is the thread local structure associated to this
     * allocator.
     */
    FastCache* attached_cache;

    /**
     * This contains the way to access all the global state and
     * configuration for the system setup.
     */
    SharedStateHandle handle;

    /**
     * The message queue needs to be accessible from other threads
     *
     * In the cross trust domain version this is the minimum amount
     * of allocator state that must be accessible to other threads.
     */
    auto* public_state()
    {
      if constexpr (SharedStateHandle::IsQueueInline)
      {
        return &remote_alloc;
      }
      else
      {
        return remote_alloc;
      }
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

    /**
     * Abstracts access to the message queue to handle different
     * layout configurations of the allocator.
     */
    auto& message_queue()
    {
      return public_state()->message_queue;
    }

    /**
     * The message queue has non-trivial initialisation as it needs to
     * be non-empty, so we prime it with a single allocation.
     */
    void init_message_queue()
    {
      // Manufacture an allocation to prime the queue
      // Using an actual allocation removes a conditional from a critical path.
      auto dummy =
        CapPtr<void, CBAlloc>(small_alloc_one(sizeof(MIN_ALLOC_SIZE)))
          .template as_static<Remote>();
      if (dummy == nullptr)
      {
        error("Critical error: Out-of-memory during initialisation.");
      }
      dummy->set_info(get_trunc_id(), size_to_sizeclass_const(MIN_ALLOC_SIZE));
      message_queue().init(dummy);
    }

    /**
     * There are a few internal corner cases where we need to allocate
     * a small object.  These are not on the fast path,
     *   - Allocating object of size zero
     *   - Allocating stub in the message queue
     * TODO: This code should probably be improved, but not perf critical.
     */
    template<ZeroMem zero_mem = NoZero>
    void* small_alloc_one(size_t size)
    {
      // Use attached cache, and fill it if it is empty.
      if (attached_cache != nullptr)
        return attached_cache->template alloc<zero_mem, SharedStateHandle>(
          size, [&](sizeclass_t sizeclass, FreeListIter* fl) {
            return small_alloc<zero_mem>(sizeclass, *fl);
          });

      auto sizeclass = size_to_sizeclass(size);
      //   stats().alloc_request(size);
      //   stats().sizeclass_alloc(sizeclass);

      // This is a debug path.  When we reallocate a message queue in
      // debug check empty, that might occur when the allocator is not attached
      // to any thread.  Hence, the following unperformant code is acceptable.
      // TODO: Potentially do something nicer.
      FreeListIter temp;
      auto r = small_alloc<zero_mem>(sizeclass, temp);
      while (!temp.empty())
      {
        // Fake statistics up.
        // stats().sizeclass_alloc(sizeclass);
        dealloc_local_object(finish_alloc_no_zero(temp.take(entropy), sizeclass));
      }
      return r;
    }

    static SNMALLOC_FAST_PATH void alloc_new_list(
      CapPtr<void, CBChunk>& bumpptr,
      FreeListIter& fast_free_list,
      size_t rsize,
      size_t slab_size,
      LocalEntropy& entropy)
    {
      auto slab_end = pointer_offset(bumpptr, slab_size + 1 - rsize);

      FreeListBuilder<false> b;
      SNMALLOC_ASSERT(b.empty());

#ifdef CHECK_CLIENT
      // Structure to represent the temporary list elements
      struct PreAllocObject
      {
        CapPtr<PreAllocObject, CBAlloc> next;
      };
      // The following code implements Sattolo's algorithm for generating
      // random cyclic permutations.  This implementation is in the opposite
      // direction, so that the original space does not need initialising.  This
      // is described as outside-in without citation on Wikipedia, appears to be
      // Folklore algorithm.

      // Note the wide bounds on curr relative to each of the ->next fields;
      // curr is not persisted once the list is built.
      CapPtr<PreAllocObject, CBChunk> curr =
        pointer_offset(bumpptr, 0).template as_static<PreAllocObject>();
      curr->next = Aal::capptr_bound<PreAllocObject, CBAlloc>(curr, rsize);

      uint16_t count = 1;
      for (curr =
             pointer_offset(curr, rsize).template as_static<PreAllocObject>();
           curr.as_void() < slab_end;
           curr =
             pointer_offset(curr, rsize).template as_static<PreAllocObject>())
      {
        size_t insert_index = entropy.sample(count);
        curr->next = std::exchange(
          pointer_offset(bumpptr, insert_index * rsize)
            .template as_static<PreAllocObject>()
            ->next,
          Aal::capptr_bound<PreAllocObject, CBAlloc>(curr, rsize));
        count++;
      }

      // Pick entry into space, and then build linked list by traversing cycle
      // to the start.  Use ->next to jump from CBArena to CBAlloc.
      auto start_index = entropy.sample(count);
      auto start_ptr = pointer_offset(bumpptr, start_index * rsize)
                         .template as_static<PreAllocObject>()
                         ->next;
      auto curr_ptr = start_ptr;
      do
      {
        b.add(FreeObject::make(curr_ptr.as_void()), entropy);
        curr_ptr = curr_ptr->next;
      } while (curr_ptr != start_ptr);
#else
      for (auto p = bumpptr; p < slab_end; p = pointer_offset(p, rsize))
      {
        b.add(Aal::capptr_bound<FreeObject, CBAlloc>(p, rsize), entropy);
      }
#endif
      // This code consumes everything up to slab_end.
      bumpptr = slab_end;

      SNMALLOC_ASSERT(!b.empty());
      b.close(fast_free_list, entropy);
    }

    SlabRecord* clear_slab(Metaslab* meta, sizeclass_t sizeclass)
    {
      FreeListIter fl;
      meta->free_queue.close(fl, entropy);
      void* p = finish_alloc_no_zero(fl.take(entropy), sizeclass);

#ifdef CHECK_CLIENT
      // Check free list is well-formed on platforms with
      // integers as pointers.
      size_t count = 1; // Already taken one above.
      while (!fl.empty())
      {
        fl.take(entropy);
        count++;
      }
      // Check the list contains all the elements
      SNMALLOC_ASSERT(
        count == snmalloc::sizeclass_to_slab_object_count(sizeclass));
#endif
      SlabRecord* slab_record = reinterpret_cast<SlabRecord*>(meta);
      // TODO: This is a capability amplification as we are saying we
      // have the whole slab.
      auto start_of_slab = pointer_align_down<void>(
        p, snmalloc::sizeclass_to_slab_size(sizeclass));
      // TODO Add bounds correctly here
      slab_record->slab = CapPtr<void, CBChunk>(start_of_slab);

#ifdef SNMALLOC_TRACING
      std::cout << "Slab " << start_of_slab << " is unused, Object sizeclass "
                << sizeclass << std::endl;
#endif
      return slab_record;
    }

    SNMALLOC_SLOW_PATH void dealloc_local_slabs(sizeclass_t sizeclass)
    {
      // Return unused slabs of sizeclass_t back to global allocator
      auto prev = &alloc_classes[sizeclass];
      auto curr = prev->get_next();
      auto next = curr->get_next();
      while (next != nullptr)
      {
        Metaslab* meta = (Metaslab*)curr;
        if (meta->needed() == 0)
        {
          prev->pop();
          auto slab_record = clear_slab(meta, sizeclass);
          SlabAllocator::dealloc(
            handle, slab_record, sizeclass_to_slab_sizeclass(sizeclass));
        }
        else
        {
          prev = curr;
        }
        curr = next;
        next = curr->get_next();
      }
    }

    /**
     * Slow path for deallocating an object locally.
     * This is either waking up a slab that was not actively being used
     * by this thread, or handling the final deallocation onto a slab,
     * so it can be reused by other threads.
     */
    SNMALLOC_SLOW_PATH void
    dealloc_local_object_slow(const MetaEntry& entry, void* p)
    {
      // TODO: Handle message queue on this path?

      Metaslab* meta = entry.get_metaslab();
      sizeclass_t sizeclass = entry.get_sizeclass();

      UNUSED(entropy);
      if (meta->is_sleeping())
      {
        // Slab has been woken up add this to the list of slabs with free space.

        //  Wake slab up.
        meta->set_not_sleeping(sizeclass);

        alloc_classes[sizeclass].insert(meta);

        // TODO increase list length
#ifdef SNMALLOC_TRACING
        std::cout << "Slab is woken up" << std::endl;
#endif

        return;
      }

      // Slab is no longer in use, return to global pool of slabs.
      dealloc_local_slabs(sizeclass);
      // TODO Increase unused count

      // TODO Check if unused above threshold

      // TODO Filter unused back to global data structure.
      UNUSED(p);
      // TODO Disable returning for now.
      // #ifdef CHECK_CLIENT
      //       // Check free list is well-formed on platforms with
      //       // integers as pointers.
      //       FreeListIter fl;
      //       meta->free_queue.close(fl, entropy);

      //       size_t count = 0;
      //       while (!fl.empty())
      //       {
      //         fl.take(entropy);
      //         count++;
      //       }
      //       // Check the list contains all the elements
      //       SNMALLOC_ASSERT(
      //         count == snmalloc::sizeclass_to_slab_object_count(sizeclass));
      // #endif

      //       meta->remove();
      //       SlabRecord* slab_record = reinterpret_cast<SlabRecord*>(meta);
      //       // TODO: This is a capability amplification as we are saying we
      //       have the
      //       // whole slab.
      //       auto start_of_slab = pointer_align_down<void>(
      //         p, snmalloc::sizeclass_to_slab_size(sizeclass));
      //       // TODO Add bounds correctly here
      //       slab_record->slab = CapPtr<void, CBChunk>(start_of_slab);
      //       SlabAllocator::dealloc(
      //         handle, slab_record, sizeclass_to_slab_sizeclass(sizeclass));
      // #ifdef SNMALLOC_TRACING
      //       std::cout << "Slab " << start_of_slab << " is unused, Object
      //       sizeclass "
      //                 << sizeclass << std::endl;
      // #endif
    }

    /**
     * Check if this allocator has messages to deallocate blocks from another
     * thread
     */
    SNMALLOC_FAST_PATH bool has_messages()
    {
      return !(message_queue().is_empty());
    }

    /**
     * Process remote frees into this allocator.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto)
    handle_message_queue_inner(Action action, Args... args)
    {
      bool need_post = false;
      for (size_t i = 0; i < REMOTE_BATCH; i++)
      {
        auto r = message_queue().dequeue();

        if (unlikely(!r.second))
          break;
#ifdef SNMALLOC_TRACING
        std::cout << "Handling remote" << std::endl;
#endif
        handle_dealloc_remote(r.first, need_post);
      }

      if (need_post)
      {
        post();
      }

      return action(args...);
    }

    /**
     * Dealloc a message either by putting for a forward, or
     * deallocating locally.
     *
     * need_post will be set to true, if capacity is exceeded.
     */
    void handle_dealloc_remote(CapPtr<Remote, CBAlloc> p, bool& need_post)
    {
      // TODO this needs to not double count stats
      // TODO this needs to not double revoke if using MTE
      // TODO batch post at the end of processing.
      // TODO thread capabilities?

      auto entry = snmalloc::BackendAllocator::get_meta_data(
        handle, snmalloc::address_cast(p));
      if (entry.get_remote() == public_state())
        dealloc_local_object(p.unsafe_capptr);
      else
      {
        if ((!need_post) && (attached_cache->capacity > 0))
          attached_cache->capacity -= sizeclass_to_size(entry.get_sizeclass());
        else
          need_post = true;
        remote_cache.template dealloc<sizeof(CoreAlloc)>(
          entry.get_remote()->trunc_id(), p.as_void());
      }
    }

  public:
    CoreAlloc(FastCache* cache, SharedStateHandle handle)
    : attached_cache(cache), handle(handle)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "Making an allocator." << std::endl;
#endif
      // Entropy must be first, so that all data-structures can use the key
      // it generates.
      // This must occur before any freelists are constructed.
      entropy.init<typename SharedStateHandle::Pal>();

      // Ignoring stats for now.
      //      stats().start();

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
     * Post deallocations onto other threads.
     *
     * Returns true if it actually performed a post,
     * and false otherwise.
     */
    SNMALLOC_FAST_PATH bool post()
    {
      // stats().remote_post();  // TODO queue not in line!
      bool sent_something = remote_cache.post<sizeof(CoreAlloc)>(
        handle, public_state()->trunc_id());
      if (attached_cache != nullptr)
        attached_cache->capacity = REMOTE_CACHE;
      return sent_something;
    }

    template<typename Action, typename... Args>
    SNMALLOC_FAST_PATH decltype(auto)
    handle_message_queue(Action action, Args... args)
    {
      // Inline the empty check, but not necessarily the full queue handling.
      if (likely(!has_messages()))
      {
        return action(args...);
      }

      return handle_message_queue_inner(action, args...);
    }

    SNMALLOC_FAST_PATH void dealloc_local_object(void* p)
    {
      auto entry = snmalloc::BackendAllocator::get_meta_data(
        handle, snmalloc::address_cast(p));
      if (likely(dealloc_local_object_fast(entry, p, entropy)))
        return;

      dealloc_local_object_slow(entry, p);
    }

    SNMALLOC_FAST_PATH static bool dealloc_local_object_fast(const MetaEntry& entry, void* p, LocalEntropy& entropy)
    {
      auto meta = entry.get_metaslab();

      SNMALLOC_ASSERT(!meta->is_unused());

      check_client(Metaslab::is_start_of_object(entry.get_sizeclass(), address_cast(p)), "Not deallocating start of an object");

      auto cp = snmalloc::CapPtr<snmalloc::FreeObject, snmalloc::CBAlloc>(
        (snmalloc::FreeObject*)p);

      // Update the head and the next pointer in the free list.
      meta->free_queue.add(cp, entropy);

      return likely(!meta->return_object());
    }

    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH void*
    small_alloc(sizeclass_t sizeclass, FreeListIter& fast_free_list)
    {
      size_t rsize = sizeclass_to_size(sizeclass);

      // Look to see if we can grab a free list.
      auto& sl = alloc_classes[sizeclass];
      if (likely(!(sl.is_empty())))
      {
        auto meta = sl.pop();
        // TODO: drop length of sl, and empty count if it was empty.
        auto p = Metaslab::alloc((Metaslab*)meta, fast_free_list, entropy, sizeclass);

        return finish_alloc<zero_mem, SharedStateHandle>(p, sizeclass);
      }
      return small_alloc_slow<zero_mem>(sizeclass, fast_free_list, rsize);
    }

    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH void* small_alloc_slow(
      sizeclass_t sizeclass, FreeListIter& fast_free_list, size_t rsize)
    {
      // No existing free list get a new slab.
      size_t slab_size = sizeclass_to_slab_size(sizeclass);
      size_t slab_sizeclass = sizeclass_to_slab_sizeclass(sizeclass);

#ifdef SNMALLOC_TRACING
      std::cout << "rsize " << rsize << std::endl;
      std::cout << "slab size " << slab_size << std::endl;
#endif

      auto [slab, meta] = snmalloc::SlabAllocator::alloc(
        handle, local_address_space, sizeclass, slab_sizeclass, slab_size, public_state());

      if (slab == nullptr)
      {
        return nullptr;
      }

      // Build a free list for the slab
      alloc_new_list(slab, fast_free_list, rsize, slab_size, entropy);

      // Set meta slab to empty.
      meta->initialise(sizeclass);

      // take an allocation from the free list
      auto p = fast_free_list.take(entropy);

      return finish_alloc<zero_mem, SharedStateHandle>(p, sizeclass);
    }

    /**
     * Flush the cached state and delayed deallocations
     *
     * Returns true if messages are sent to other threads.
     */
    bool flush(bool destroy_queue = false)
    {
      // Drain the caches back to the originating allocator
      if (attached_cache != nullptr)
        attached_cache->flush([&](auto p) { dealloc_local_object(p); });

      if (destroy_queue)
      {
        CapPtr<Remote, CBAlloc> p = message_queue().destroy();

        while (p != nullptr)
        {
          bool need_post = true; // Always going to post, so ignore.
          auto n = p->non_atomic_next;
          handle_dealloc_remote(p, need_post);
          p = n;
        }
      }
      else
      {
        // Process incoming message queue
        // Loop as normally only processes a batch
        while (has_messages())
          handle_message_queue([]() {});
      }

      // Flush remote cache at this point too.
      // do this after handling messages as we
      // may be forwarding messages.
      return post();
    }

    /**
     * If result parameter is non-null, then false is assigned into the
     * the location pointed to by result if this allocator is non-empty.
     *
     * If result pointer is null, then this code raises a Pal::error on the
     * particular check that fails, if any do fail.
     *
     * Do not run this while other thread could be deallocating as the
     * message queue invariant is temporarily broken.
     */
    bool debug_is_empty(bool* result)
    {
      auto test = [&result](auto& queue) {
        if (!queue.is_empty())
        {
          if (result != nullptr)
            *result = false;
          else
          //TODO, reenable:
{}//            error("debug_is_empty: found non-empty allocator");
        }
      };

      bool sent_something = flush(true);

      for (auto& alloc_class : alloc_classes)
      {
        test(alloc_class);
      }

      // Place the static stub message on the queue.
      init_message_queue();

      return sent_something;
    }
  };
}