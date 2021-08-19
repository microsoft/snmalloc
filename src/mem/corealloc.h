#pragma once

#include "../ds/defines.h"
#include "allocconfig.h"
#include "localcache.h"
#include "metaslab.h"
#include "pool.h"
#include "remotecache.h"
#include "sizeclasstable.h"
#include "slaballocator.h"

namespace snmalloc
{
  /**
   * Empty class used as the superclass for `CoreAllocator` when it does not
   * opt into pool allocation.  This class exists because `std::conditional`
   * (or other equivalent features in C++) can choose between options for
   * superclasses but they cannot choose whether a class has a superclass.
   * Setting the superclass to an empty class is equivalent to no superclass.
   */
  class NotPoolAllocated
  {};

  /**
   * The core, stateful, part of a memory allocator.  Each `LocalAllocator`
   * owns one `CoreAllocator` once it is initialised.
   *
   * The template parameter provides all of the global configuration for this
   * instantiation of snmalloc.  This includes three options that apply to this
   * class:
   *
   * - `CoreAllocIsPoolAllocated` defines whether this `CoreAlloc`
   *   configuration should support pool allocation.  This defaults to true but
   *   a configuration that allocates allocators eagerly may opt out.
   * - `CoreAllocOwnsLocalState` defines whether the `CoreAllocator` owns the
   *   associated `LocalState` object.  If this is true (the default) then
   *   `CoreAllocator` embeds the LocalState object.  If this is set to false
   *   then a `LocalState` object must be provided to the constructor.  This
   *   allows external code to provide explicit configuration of the address
   *   range managed by this object.
   * - `IsQueueInline` (defaults to true) defines whether the message queue
   *   (`RemoteAllocator`) for this class is inline or provided externally.  If
   *   provided externally, then it must be set explicitly with
   *   `init_message_queue`.
   */
  template<typename SharedStateHandle>
  class CoreAllocator : public std::conditional_t<
                          SharedStateHandle::Options.CoreAllocIsPoolAllocated,
                          Pooled<CoreAllocator<SharedStateHandle>>,
                          NotPoolAllocated>
  {
    template<typename SharedStateHandle2>
    friend class LocalAllocator;

    /**
     * Per size class list of active slabs for this allocator.
     */
    MetaslabCache alloc_classes[NUM_SIZECLASSES];

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
      SharedStateHandle::Options.IsQueueInline,
      RemoteAllocator,
      RemoteAllocator*>
      remote_alloc;

    /**
     * The type used local state.  This is defined by the back end.
     */
    using LocalState = typename SharedStateHandle::LocalState;

    /**
     * A local area of address space managed by this allocator.
     * Used to reduce calls on the global address space.  This is inline if the
     * core allocator owns the local state or indirect if it is owned
     * externally.
     */
    std::conditional_t<
      SharedStateHandle::Options.CoreAllocOwnsLocalState,
      LocalState,
      LocalState*>
      backend_state;

    /**
     * This is the thread local structure associated to this
     * allocator.
     */
    LocalCache* attached_cache;

    /**
     * The message queue needs to be accessible from other threads
     *
     * In the cross trust domain version this is the minimum amount
     * of allocator state that must be accessible to other threads.
     */
    auto* public_state()
    {
      if constexpr (SharedStateHandle::Options.IsQueueInline)
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
      return *public_state();
    }

    /**
     * The message queue has non-trivial initialisation as it needs to
     * be non-empty, so we prime it with a single allocation.
     */
    void init_message_queue()
    {
      // Manufacture an allocation to prime the queue
      // Using an actual allocation removes a conditional from a critical path.
      auto dummy = CapPtr<void, CBAlloc>(small_alloc_one(MIN_ALLOC_SIZE))
                     .template as_static<FreeObject>();
      if (dummy == nullptr)
      {
        error("Critical error: Out-of-memory during initialisation.");
      }
      message_queue().init(dummy);
    }

    /**
     * There are a few internal corner cases where we need to allocate
     * a small object.  These are not on the fast path,
     *   - Allocating stub in the message queue
     * Note this is not performance critical as very infrequently called.
     */
    void* small_alloc_one(size_t size)
    {
      SNMALLOC_ASSERT(attached_cache != nullptr);
      // Use attached cache, and fill it if it is empty.
      return attached_cache->template alloc<NoZero, SharedStateHandle>(
        size, [&](sizeclass_t sizeclass, FreeListIter* fl) {
          return small_alloc<NoZero>(sizeclass, *fl);
        });
    }

    static SNMALLOC_FAST_PATH void alloc_new_list(
      CapPtr<void, CBChunk>& bumpptr,
      FreeListIter& fast_free_list,
      size_t rsize,
      size_t slab_size,
      LocalEntropy& entropy)
    {
      auto slab_end = pointer_offset(bumpptr, slab_size + 1 - rsize);

      auto& key = entropy.get_free_list_key();

      FreeListBuilder<false> b;
      SNMALLOC_ASSERT(b.empty());

#ifdef SNMALLOC_CHECK_CLIENT
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
        b.add(FreeObject::make(curr_ptr.as_void()), key);
        curr_ptr = curr_ptr->next;
      } while (curr_ptr != start_ptr);
#else
      auto p = bumpptr;
      do
      {
        b.add(Aal::capptr_bound<FreeObject, CBAlloc>(p, rsize), key);
        p = pointer_offset(p, rsize);
      } while (p < slab_end);
#endif
      // This code consumes everything up to slab_end.
      bumpptr = slab_end;

      SNMALLOC_ASSERT(!b.empty());
      b.close(fast_free_list, key);
    }

    ChunkRecord* clear_slab(Metaslab* meta, sizeclass_t sizeclass)
    {
      auto& key = entropy.get_free_list_key();
      FreeListIter fl;
      meta->free_queue.close(fl, key);
      void* p = finish_alloc_no_zero(fl.take(key), sizeclass);

#ifdef SNMALLOC_CHECK_CLIENT
      // Check free list is well-formed on platforms with
      // integers as pointers.
      size_t count = 1; // Already taken one above.
      while (!fl.empty())
      {
        fl.take(key);
        count++;
      }
      // Check the list contains all the elements
      SNMALLOC_ASSERT(
        count == snmalloc::sizeclass_to_slab_object_count(sizeclass));
#endif
      ChunkRecord* chunk_record = reinterpret_cast<ChunkRecord*>(meta);
      // TODO: This is a capability amplification as we are saying we
      // have the whole chunk.
      auto start_of_slab = pointer_align_down<void>(
        p, snmalloc::sizeclass_to_slab_size(sizeclass));
      // TODO Add bounds correctly here
      chunk_record->chunk = CapPtr<void, CBChunk>(start_of_slab);

#ifdef SNMALLOC_TRACING
      std::cout << "Slab " << start_of_slab << " is unused, Object sizeclass "
                << sizeclass << std::endl;
#endif
      return chunk_record;
    }

    SNMALLOC_SLOW_PATH void dealloc_local_slabs(sizeclass_t sizeclass)
    {
      // Return unused slabs of sizeclass_t back to global allocator
      SlabLink* prev = &alloc_classes[sizeclass];
      auto curr = prev->get_next();
      while (curr != nullptr)
      {
        auto nxt = curr->get_next();
        auto meta = reinterpret_cast<Metaslab*>(curr);
        if (meta->needed() == 0)
        {
          prev->pop();
          alloc_classes[sizeclass].length--;
          alloc_classes[sizeclass].unused--;

          // TODO delay the clear to the next user of the slab, or teardown so
          // don't touch the cache lines at this point in check_client.
          auto chunk_record = clear_slab(meta, sizeclass);
          ChunkAllocator::dealloc<SharedStateHandle>(
            get_backend_local_state(),
            chunk_record,
            sizeclass_to_slab_sizeclass(sizeclass));
        }
        else
        {
          prev = curr;
        }
        curr = nxt;
      }
    }

    /**
     * Slow path for deallocating an object locally.
     * This is either waking up a slab that was not actively being used
     * by this thread, or handling the final deallocation onto a slab,
     * so it can be reused by other threads.
     */
    SNMALLOC_SLOW_PATH void dealloc_local_object_slow(const MetaEntry& entry)
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
        alloc_classes[sizeclass].length++;

#ifdef SNMALLOC_TRACING
        std::cout << "Slab is woken up" << std::endl;
#endif

        return;
      }

      alloc_classes[sizeclass].unused++;

      // If we have several slabs, and it isn't too expensive as a proportion
      // return to the global pool.
      if (
        (alloc_classes[sizeclass].unused > 2) &&
        (alloc_classes[sizeclass].unused >
         (alloc_classes[sizeclass].length >> 2)))
      {
        dealloc_local_slabs(sizeclass);
      }
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
        auto p = message_queue().peek();
        auto& entry =
          SharedStateHandle::get_meta_data(snmalloc::address_cast(p));

        auto r = message_queue().dequeue(key_global);

        if (unlikely(!r.second))
          break;
#ifdef SNMALLOC_TRACING
        std::cout << "Handling remote" << std::endl;
#endif
        handle_dealloc_remote(entry, p, need_post);
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
    void handle_dealloc_remote(
      const MetaEntry& entry, CapPtr<FreeObject, CBAlloc> p, bool& need_post)
    {
      // TODO this needs to not double count stats
      // TODO this needs to not double revoke if using MTE
      // TODO thread capabilities?

      if (likely(entry.get_remote() == public_state()))
      {
        if (likely(dealloc_local_object_fast(entry, p.unsafe_ptr(), entropy)))
          return;

        dealloc_local_object_slow(entry);
      }
      else
      {
        if (
          !need_post &&
          !attached_cache->remote_dealloc_cache.reserve_space(entry))
          need_post = true;
        attached_cache->remote_dealloc_cache
          .template dealloc<sizeof(CoreAllocator)>(
            entry.get_remote()->trunc_id(), p.as_void(), key_global);
      }
    }

    /**
     * Initialiser, shared code between the constructors for different
     * configurations.
     */
    void init()
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

      if constexpr (SharedStateHandle::Options.IsQueueInline)
      {
        init_message_queue();
        message_queue().invariant();
      }

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

  public:
    /**
     * Constructor for the case that the core allocator owns the local state.
     * SFINAE disabled if the allocator does not own the local state.
     */
    template<
      typename Config = SharedStateHandle,
      typename = std::enable_if_t<Config::Options.CoreAllocOwnsLocalState>>
    CoreAllocator(LocalCache* cache) : attached_cache(cache)
    {
      init();
    }

    /**
     * Constructor for the case that the core allocator does not owns the local
     * state. SFINAE disabled if the allocator does own the local state.
     */
    template<
      typename Config = SharedStateHandle,
      typename = std::enable_if_t<!Config::Options.CoreAllocOwnsLocalState>>
    CoreAllocator(LocalCache* cache, LocalState* backend = nullptr)
    : backend_state(backend), attached_cache(cache)
    {
      init();
    }

    /**
     * If the message queue is not inline, provide it.  This will then
     * configure the message queue for use.
     */
    template<bool InlineQueue = SharedStateHandle::Options.IsQueueInline>
    std::enable_if_t<!InlineQueue> init_message_queue(RemoteAllocator* q)
    {
      remote_alloc = q;
      init_message_queue();
      message_queue().invariant();
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
      bool sent_something = attached_cache->remote_dealloc_cache
                              .post<sizeof(CoreAllocator), SharedStateHandle>(
                                public_state()->trunc_id(), key_global);

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
      auto entry = SharedStateHandle::get_meta_data(snmalloc::address_cast(p));
      if (likely(dealloc_local_object_fast(entry, p, entropy)))
        return;

      dealloc_local_object_slow(entry);
    }

    SNMALLOC_FAST_PATH static bool dealloc_local_object_fast(
      const MetaEntry& entry, void* p, LocalEntropy& entropy)
    {
      auto meta = entry.get_metaslab();

      SNMALLOC_ASSERT(!meta->is_unused());

      check_client(
        Metaslab::is_start_of_object(entry.get_sizeclass(), address_cast(p)),
        "Not deallocating start of an object");

      auto cp = CapPtr<FreeObject, CBAlloc>(reinterpret_cast<FreeObject*>(p));

      auto& key = entropy.get_free_list_key();

      // Update the head and the next pointer in the free list.
      meta->free_queue.add(cp, key, entropy);

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
        auto meta = reinterpret_cast<Metaslab*>(sl.pop());
        // Drop length of sl, and empty count if it was empty.
        alloc_classes[sizeclass].length--;
        if (meta->needed() == 0)
          alloc_classes[sizeclass].unused--;

        auto p = Metaslab::alloc(meta, fast_free_list, entropy, sizeclass);

        return finish_alloc<zero_mem, SharedStateHandle>(p, sizeclass);
      }
      return small_alloc_slow<zero_mem>(sizeclass, fast_free_list, rsize);
    }

    /**
     * Accessor for the local state.  This hides whether the local state is
     * stored inline or provided externally from the rest of the code.
     */
    SNMALLOC_FAST_PATH
    LocalState& get_backend_local_state()
    {
      if constexpr (SharedStateHandle::Options.CoreAllocOwnsLocalState)
      {
        return backend_state;
      }
      else
      {
        SNMALLOC_ASSERT(backend_state);
        return *backend_state;
      }
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

      auto [slab, meta] =
        snmalloc::ChunkAllocator::alloc_chunk<SharedStateHandle>(
          get_backend_local_state(),
          sizeclass,
          slab_sizeclass,
          slab_size,
          public_state());

      if (slab == nullptr)
      {
        return nullptr;
      }

      // Build a free list for the slab
      alloc_new_list(slab, fast_free_list, rsize, slab_size, entropy);

      // Set meta slab to empty.
      meta->initialise(sizeclass);

      auto& key = entropy.get_free_list_key();

      // take an allocation from the free list
      auto p = fast_free_list.take(key);

      return finish_alloc<zero_mem, SharedStateHandle>(p, sizeclass);
    }

    /**
     * Flush the cached state and delayed deallocations
     *
     * Returns true if messages are sent to other threads.
     */
    bool flush(bool destroy_queue = false)
    {
      SNMALLOC_ASSERT(attached_cache != nullptr);

      if (destroy_queue)
      {
        auto p = message_queue().destroy();

        while (p != nullptr)
        {
          bool need_post = true; // Always going to post, so ignore.
          auto n = p->atomic_read_next(key_global);
          auto& entry =
            SharedStateHandle::get_meta_data(snmalloc::address_cast(p));
          handle_dealloc_remote(entry, p, need_post);
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

      auto posted =
        attached_cache->flush<sizeof(CoreAllocator), SharedStateHandle>(
          [&](auto p) { dealloc_local_object(p); });

      // We may now have unused slabs, return to the global allocator.
      for (sizeclass_t sizeclass = 0; sizeclass < NUM_SIZECLASSES; sizeclass++)
      {
        dealloc_local_slabs(sizeclass);
      }

      return posted;
    }

    // This allows the caching layer to be attached to an underlying
    // allocator instance.
    void attach(LocalCache* c)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "Attach cache to " << this << std::endl;
#endif
      attached_cache = c;

      // Set up secrets.
      c->entropy = entropy;

      // Set up remote allocator.
      c->remote_allocator = public_state();

      // Set up remote cache.
      c->remote_dealloc_cache.init();
    }

    /**
     * Performs the work of checking if empty under the assumption that
     * a local cache has been attached.
     */
    bool debug_is_empty_impl(bool* result)
    {
      auto test = [&result](auto& queue) {
        if (!queue.is_empty())
        {
          auto curr = reinterpret_cast<Metaslab*>(queue.get_next());
          while (curr != nullptr)
          {
            if (curr->needed() != 0)
            {
              if (result != nullptr)
                *result = false;
              else
                error("debug_is_empty: found non-empty allocator");
            }
            curr = reinterpret_cast<Metaslab*>(curr->get_next());
          }
        }
      };

      bool sent_something = flush(true);

      for (auto& alloc_class : alloc_classes)
      {
        test(alloc_class);
      }

      // Place the static stub message on the queue.
      init_message_queue();

#ifdef SNMALLOC_TRACING
      std::cout << "debug_is_empty - done" << std::endl;
#endif
      return sent_something;
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
#ifdef SNMALLOC_TRACING
      std::cout << "debug_is_empty" << std::endl;
#endif
      if (attached_cache == nullptr)
      {
        // We need a cache to perform some operations, so set one up
        // temporarily
        LocalCache temp(public_state());
        attach(&temp);
#ifdef SNMALLOC_TRACING
        std::cout << "debug_is_empty - attach a cache" << std::endl;
#endif
        auto sent_something = debug_is_empty_impl(result);

        // Remove cache from the allocator
        flush();
        attached_cache = nullptr;
        return sent_something;
      }

      return debug_is_empty_impl(result);
    }
  };
} // namespace snmalloc
