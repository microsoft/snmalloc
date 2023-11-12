#pragma once

#include "../ds/ds.h"
#include "localcache.h"
#include "metadata.h"
#include "pool.h"
#include "remotecache.h"
#include "sizeclasstable.h"
#include "ticker.h"

namespace snmalloc
{
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
  template<SNMALLOC_CONCEPT(IsConfigLazy) Config>
  class CoreAllocator : public std::conditional_t<
                          Config::Options.CoreAllocIsPoolAllocated,
                          Pooled<CoreAllocator<Config>>,
                          Empty>
  {
    template<SNMALLOC_CONCEPT(IsConfig)>
    friend class LocalAllocator;

    /**
     * Define local names for specialised versions of various types that are
     * specialised for the back-end that we are using.
     * @{
     */
    using BackendSlabMetadata = typename Config::Backend::SlabMetadata;
    using PagemapEntry = typename Config::PagemapEntry;
    /// }@

    /**
     * Per size class list of active slabs for this allocator.
     */
    struct SlabMetadataCache
    {
      SeqSet<BackendSlabMetadata> available{};

      uint16_t unused = 0;
      uint16_t length = 0;
    } alloc_classes[NUM_SMALL_SIZECLASSES]{};

    /**
     * The set of all slabs and large allocations
     * from this allocator that are full or almost full.
     */
    SeqSet<BackendSlabMetadata> laden{};

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
      Config::Options.IsQueueInline,
      RemoteAllocator,
      RemoteAllocator*>
      remote_alloc;

    /**
     * The type used local state.  This is defined by the back end.
     */
    using LocalState = typename Config::LocalState;

    /**
     * A local area of address space managed by this allocator.
     * Used to reduce calls on the global address space.  This is inline if the
     * core allocator owns the local state or indirect if it is owned
     * externally.
     */
    std::conditional_t<
      Config::Options.CoreAllocOwnsLocalState,
      LocalState,
      LocalState*>
      backend_state;

    /**
     * This is the thread local structure associated to this
     * allocator.
     */
    LocalCache* attached_cache;

    /**
     * Ticker to query the clock regularly at a lower cost.
     */
    Ticker<typename Config::Pal> ticker;

    /**
     * The message queue needs to be accessible from other threads
     *
     * In the cross trust domain version this is the minimum amount
     * of allocator state that must be accessible to other threads.
     */
    auto* public_state()
    {
      if constexpr (Config::Options.IsQueueInline)
      {
        return &remote_alloc;
      }
      else
      {
        return remote_alloc;
      }
    }

    /**
     * Return a pointer to the backend state.
     */
    LocalState* backend_state_ptr()
    {
      if constexpr (Config::Options.CoreAllocOwnsLocalState)
      {
        return &backend_state;
      }
      else
      {
        return backend_state;
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
     * be non-empty, so we prime it with a fake allocation.
     */
    void init_message_queue()
    {
      message_queue().init();
    }

    static SNMALLOC_FAST_PATH void alloc_new_list(
      capptr::Chunk<void>& bumpptr,
      BackendSlabMetadata* meta,
      size_t rsize,
      size_t slab_size,
      LocalEntropy& entropy)
    {
      auto slab_end = pointer_offset(bumpptr, slab_size + 1 - rsize);

      auto& key = entropy.get_free_list_key();

      auto& b = meta->free_queue;

      if constexpr (mitigations(random_initial))
      {
        // Structure to represent the temporary list elements
        struct PreAllocObject
        {
          capptr::AllocFull<PreAllocObject> next;
        };
        // The following code implements Sattolo's algorithm for generating
        // random cyclic permutations.  This implementation is in the opposite
        // direction, so that the original space does not need initialising.
        // This is described as outside-in without citation on Wikipedia,
        // appears to be Folklore algorithm.

        // Note the wide bounds on curr relative to each of the ->next fields;
        // curr is not persisted once the list is built.
        capptr::Chunk<PreAllocObject> curr =
          pointer_offset(bumpptr, 0).template as_static<PreAllocObject>();
        curr->next =
          Aal::capptr_bound<PreAllocObject, capptr::bounds::AllocFull>(
            curr, rsize);

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
            Aal::capptr_bound<PreAllocObject, capptr::bounds::AllocFull>(
              curr, rsize));
          count++;
        }

        // Pick entry into space, and then build linked list by traversing cycle
        // to the start.  Use ->next to jump from Chunk to Alloc.
        auto start_index = entropy.sample(count);
        auto start_ptr = pointer_offset(bumpptr, start_index * rsize)
                           .template as_static<PreAllocObject>()
                           ->next;
        auto curr_ptr = start_ptr;
        do
        {
          b.add(
            // Here begins our treatment of the heap as containing Wild pointers
            freelist::Object::make<capptr::bounds::AllocWild>(
              capptr_to_user_address_control(curr_ptr.as_void())),
            key,
            entropy);
          curr_ptr = curr_ptr->next;
        } while (curr_ptr != start_ptr);
      }
      else
      {
        auto p = bumpptr;
        do
        {
          b.add(
            // Here begins our treatment of the heap as containing Wild pointers
            freelist::Object::make<capptr::bounds::AllocWild>(
              capptr_to_user_address_control(
                Aal::capptr_bound<void, capptr::bounds::AllocFull>(
                  p.as_void(), rsize))),
            key,
            entropy);
          p = pointer_offset(p, rsize);
        } while (p < slab_end);
      }
      // This code consumes everything up to slab_end.
      bumpptr = slab_end;
    }

    capptr::Alloc<void>
    clear_slab(BackendSlabMetadata* meta, smallsizeclass_t sizeclass)
    {
      auto& key = entropy.get_free_list_key();
      freelist::Iter<> fl;
      auto more = meta->free_queue.close(fl, key);
      UNUSED(more);
      auto local_state = backend_state_ptr();
      auto domesticate = [local_state](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<Config>(local_state, p);
                           };
      capptr::Alloc<void> p =
        finish_alloc_no_zero(fl.take(key, domesticate), sizeclass);

      // If clear_meta is requested, we should also walk the free list to clear
      // it.
      // TODO: we could optimise the clear_meta case to not walk the free list
      // and instead just clear the whole slab, but that requires amplification.
      if constexpr (
        mitigations(freelist_teardown_validate) || mitigations(clear_meta))
      {
        // Check free list is well-formed on platforms with
        // integers as pointers.
        size_t count = 1; // Already taken one above.
        while (!fl.empty())
        {
          fl.take(key, domesticate);
          count++;
        }
        // Check the list contains all the elements
        SNMALLOC_CHECK(
          (count + more) ==
          snmalloc::sizeclass_to_slab_object_count(sizeclass));

        if (more > 0)
        {
          auto no_more = meta->free_queue.close(fl, key);
          SNMALLOC_ASSERT(no_more == 0);
          UNUSED(no_more);

          while (!fl.empty())
          {
            fl.take(key, domesticate);
            count++;
          }
        }
        SNMALLOC_CHECK(
          count == snmalloc::sizeclass_to_slab_object_count(sizeclass));
      }
      auto start_of_slab = pointer_align_down<void>(
        p, snmalloc::sizeclass_to_slab_size(sizeclass));

#ifdef SNMALLOC_TRACING
      message<1024>(
        "Slab {}  is unused, Object sizeclass {}",
        start_of_slab.unsafe_ptr(),
        sizeclass);
#endif
      return start_of_slab;
    }

    template<bool check_slabs = false>
    SNMALLOC_SLOW_PATH void dealloc_local_slabs(smallsizeclass_t sizeclass)
    {
      // Return unused slabs of sizeclass_t back to global allocator
      alloc_classes[sizeclass].available.iterate([this, sizeclass](auto* meta) {
        auto domesticate =
          [this](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
            auto res = capptr_domesticate<Config>(backend_state_ptr(), p);
#ifdef SNMALLOC_TRACING
            if (res.unsafe_ptr() != p.unsafe_ptr())
              printf(
                "Domesticated %p to %p!\n", p.unsafe_ptr(), res.unsafe_ptr());
#endif
            return res;
          };

        if (meta->needed() != 0)
        {
          if (check_slabs)
          {
            meta->free_queue.validate(entropy.get_free_list_key(), domesticate);
          }
          return;
        }

        alloc_classes[sizeclass].length--;
        alloc_classes[sizeclass].unused--;

        // Remove from the list.  This must be done before dealloc chunk
        // as that may corrupt the node.
        meta->node.remove();

        // TODO delay the clear to the next user of the slab, or teardown so
        // don't touch the cache lines at this point in snmalloc_check_client.
        auto start = clear_slab(meta, sizeclass);

        Config::Backend::dealloc_chunk(
          get_backend_local_state(),
          *meta,
          start,
          sizeclass_to_slab_size(sizeclass));
      });
    }

    /**
     * Slow path for deallocating an object locally.
     * This is either waking up a slab that was not actively being used
     * by this thread, or handling the final deallocation onto a slab,
     * so it can be reused by other threads.
     */
    SNMALLOC_SLOW_PATH void
    dealloc_local_object_slow(capptr::Alloc<void> p, const PagemapEntry& entry)
    {
      // TODO: Handle message queue on this path?

      auto* meta = entry.get_slab_metadata();

      if (meta->is_large())
      {
        // Handle large deallocation here.
        size_t entry_sizeclass = entry.get_sizeclass().as_large();
        size_t size = bits::one_at_bit(entry_sizeclass);

#ifdef SNMALLOC_TRACING
        message<1024>("Large deallocation: {}", size);
#else
        UNUSED(size);
#endif

        // Remove from set of fully used slabs.
        meta->node.remove();

        Config::Backend::dealloc_chunk(
          get_backend_local_state(), *meta, p, size);

        return;
      }

      smallsizeclass_t sizeclass = entry.get_sizeclass().as_small();

      UNUSED(entropy);
      if (meta->is_sleeping())
      {
        // Slab has been woken up add this to the list of slabs with free space.

        //  Wake slab up.
        meta->set_not_sleeping(sizeclass);

        // Remove from set of fully used slabs.
        meta->node.remove();

        alloc_classes[sizeclass].available.insert(meta);
        alloc_classes[sizeclass].length++;

#ifdef SNMALLOC_TRACING
        message<1024>("Slab is woken up");
#endif

        ticker.check_tick();
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
      ticker.check_tick();
    }

    /**
     * Check if this allocator has messages to deallocate blocks from another
     * thread
     */
    SNMALLOC_FAST_PATH bool has_messages()
    {
      auto local_state = backend_state_ptr();
      auto domesticate_head =
        [local_state](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
          if constexpr (Config::Options.QueueHeadsAreTame)
          {
            UNUSED(local_state);
            return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
          }
          else
          {
            return capptr_domesticate<Config>(local_state, p);
          }
        };
      auto domesticate_queue =
        [local_state](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
          return capptr_domesticate<Config>(local_state, p);
        };

      return message_queue().can_dequeue(domesticate_head, domesticate_queue);
    }

    /**
     * Process remote frees into this allocator.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto)
    handle_message_queue_inner(Action action, Args... args)
    {
      bool need_post = false;
      auto local_state = backend_state_ptr();
      auto domesticate = [local_state](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<Config>(local_state, p);
                           };
      auto cb = [this,
                 &need_post](freelist::HeadPtr msg) SNMALLOC_FAST_PATH_LAMBDA {
#ifdef SNMALLOC_TRACING
        message<1024>("Handling remote");
#endif

        auto& entry =
          Config::Backend::template get_metaentry(snmalloc::address_cast(msg));

        handle_dealloc_remote(entry, msg.as_void(), need_post);

        return true;
      };

      if constexpr (Config::Options.QueueHeadsAreTame)
      {
        /*
         * The front of the queue has already been validated; just change the
         * annotating type.
         */
        auto domesticate_first =
          [](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
            return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
          };
        message_queue().dequeue(domesticate_first, domesticate, cb);
      }
      else
      {
        message_queue().dequeue(domesticate, domesticate, cb);
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
      const PagemapEntry& entry,
      CapPtr<void, capptr::bounds::Alloc> p,
      bool& need_post)
    {
      // TODO this needs to not double count stats
      // TODO this needs to not double revoke if using MTE
      // TODO thread capabilities?

      if (SNMALLOC_LIKELY(entry.get_remote() == public_state()))
      {
        if (SNMALLOC_LIKELY(
              dealloc_local_object_fast(entry, p.as_void(), entropy)))
          return;

        dealloc_local_object_slow(p, entry);
      }
      else
      {
        if (
          !need_post &&
          !attached_cache->remote_dealloc_cache.reserve_space(entry))
          need_post = true;
        attached_cache->remote_dealloc_cache
          .template dealloc<sizeof(CoreAllocator)>(
            entry.get_remote()->trunc_id(), p.as_void());
      }
    }

    /**
     * Initialiser, shared code between the constructors for different
     * configurations.
     *
     * spare is the amount of space directly after the allocator that is
     * reserved as meta-data, but is not required by this CoreAllocator.
     */
    void init(Range<capptr::bounds::Alloc>& spare)
    {
#ifdef SNMALLOC_TRACING
      message<1024>("Making an allocator.");
#endif
      // Entropy must be first, so that all data-structures can use the key
      // it generates.
      // This must occur before any freelists are constructed.
      entropy.init<typename Config::Pal>();

      if (spare.length != 0)
      {
        /*
         * Seed this frontend's private metadata allocation cache with any
         * excess space from the metadata allocation holding the frontend
         * Allocator object itself.  This alleviates thundering herd
         * contention on the backend during startup: each slab opened now
         * makes one trip to the backend, for the slab itself, rather than
         * two, for the slab and its metadata.
         */
        Config::Backend::dealloc_meta_data(
          get_backend_local_state(), spare.base, spare.length);
      }

      // Ignoring stats for now.
      //      stats().start();

      if constexpr (Config::Options.IsQueueInline)
      {
        init_message_queue();
        message_queue().invariant();
      }

      if constexpr (DEBUG)
      {
        for (smallsizeclass_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
        {
          size_t size = sizeclass_to_size(i);
          smallsizeclass_t sc1 = size_to_sizeclass(size);
          smallsizeclass_t sc2 = size_to_sizeclass_const(size);
          size_t size1 = sizeclass_to_size(sc1);
          size_t size2 = sizeclass_to_size(sc2);

          SNMALLOC_CHECK(sc1 == i);
          SNMALLOC_CHECK(sc1 == sc2);
          SNMALLOC_CHECK(size1 == size);
          SNMALLOC_CHECK(size1 == size2);
        }
      }
    }

  public:
    /**
     * Constructor for the case that the core allocator owns the local state.
     * SFINAE disabled if the allocator does not own the local state.
     *
     * spare is the amount of space directly after the allocator that is
     * reserved as meta-data, but is not required by this CoreAllocator.
     */
    template<
      typename Config_ = Config,
      typename = std::enable_if_t<Config_::Options.CoreAllocOwnsLocalState>>
    CoreAllocator(Range<capptr::bounds::Alloc>& spare)
    {
      init(spare);
    }

    /**
     * Constructor for the case that the core allocator does not owns the local
     * state. SFINAE disabled if the allocator does own the local state.
     *
     * spare is the amount of space directly after the allocator that is
     * reserved as meta-data, but is not required by this CoreAllocator.
     */
    template<
      typename Config_ = Config,
      typename = std::enable_if_t<!Config_::Options.CoreAllocOwnsLocalState>>
    CoreAllocator(
      Range<capptr::bounds::Alloc>& spare,
      LocalCache* cache,
      LocalState* backend = nullptr)
    : backend_state(backend), attached_cache(cache)
    {
      init(spare);
    }

    /**
     * If the message queue is not inline, provide it.  This will then
     * configure the message queue for use.
     */
    template<bool InlineQueue = Config::Options.IsQueueInline>
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
      bool sent_something =
        attached_cache->remote_dealloc_cache
          .post<sizeof(CoreAllocator), Config>(
            backend_state_ptr(), public_state()->trunc_id());

      return sent_something;
    }

    template<typename Action, typename... Args>
    SNMALLOC_FAST_PATH decltype(auto)
    handle_message_queue(Action action, Args... args)
    {
      // Inline the empty check, but not necessarily the full queue handling.
      if (SNMALLOC_LIKELY(!has_messages()))
      {
        return action(args...);
      }

      return handle_message_queue_inner(action, args...);
    }

    SNMALLOC_FAST_PATH void
    dealloc_local_object(CapPtr<void, capptr::bounds::Alloc> p)
    {
      // PagemapEntry-s seen here are expected to have meaningful Remote
      // pointers
      auto& entry =
        Config::Backend::template get_metaentry(snmalloc::address_cast(p));
      if (SNMALLOC_LIKELY(dealloc_local_object_fast(entry, p, entropy)))
        return;

      dealloc_local_object_slow(p, entry);
    }

    SNMALLOC_FAST_PATH static bool dealloc_local_object_fast(
      const PagemapEntry& entry,
      CapPtr<void, capptr::bounds::Alloc> p,
      LocalEntropy& entropy)
    {
      auto meta = entry.get_slab_metadata();

      SNMALLOC_ASSERT(!meta->is_unused());

      snmalloc_check_client(
        mitigations(sanity_checks),
        is_start_of_object(entry.get_sizeclass(), address_cast(p)),
        "Not deallocating start of an object");

      auto cp = p.as_static<freelist::Object::T<>>();

      auto& key = entropy.get_free_list_key();

      // Update the head and the next pointer in the free list.
      meta->free_queue.add(cp, key, entropy);

      return SNMALLOC_LIKELY(!meta->return_object());
    }

    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH capptr::Alloc<void>
    small_alloc(smallsizeclass_t sizeclass, freelist::Iter<>& fast_free_list)
    {
      // Look to see if we can grab a free list.
      auto& sl = alloc_classes[sizeclass].available;
      if (SNMALLOC_LIKELY(alloc_classes[sizeclass].length > 0))
      {
        if constexpr (mitigations(random_extra_slab))
        {
          // Occassionally don't use the last list.
          if (SNMALLOC_UNLIKELY(alloc_classes[sizeclass].length == 1))
          {
            if (entropy.next_bit() == 0)
              return small_alloc_slow<zero_mem>(sizeclass, fast_free_list);
          }
        }

        // Mitigations use LIFO to increase time to reuse.
        auto meta = sl.template pop<!mitigations(reuse_LIFO)>();
        // Drop length of sl, and empty count if it was empty.
        alloc_classes[sizeclass].length--;
        if (meta->needed() == 0)
          alloc_classes[sizeclass].unused--;

        auto domesticate =
          [this](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
            return capptr_domesticate<Config>(backend_state_ptr(), p);
          };
        auto [p, still_active] = BackendSlabMetadata::alloc_free_list(
          domesticate, meta, fast_free_list, entropy, sizeclass);

        if (still_active)
        {
          alloc_classes[sizeclass].length++;
          sl.insert(meta);
        }
        else
        {
          laden.insert(meta);
        }

        auto r = finish_alloc<zero_mem, Config>(p, sizeclass);
        return ticker.check_tick(r);
      }
      return small_alloc_slow<zero_mem>(sizeclass, fast_free_list);
    }

    /**
     * Accessor for the local state.  This hides whether the local state is
     * stored inline or provided externally from the rest of the code.
     */
    SNMALLOC_FAST_PATH
    LocalState& get_backend_local_state()
    {
      if constexpr (Config::Options.CoreAllocOwnsLocalState)
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
    SNMALLOC_SLOW_PATH capptr::Alloc<void> small_alloc_slow(
      smallsizeclass_t sizeclass, freelist::Iter<>& fast_free_list)
    {
      size_t rsize = sizeclass_to_size(sizeclass);

      // No existing free list get a new slab.
      size_t slab_size = sizeclass_to_slab_size(sizeclass);

#ifdef SNMALLOC_TRACING
      message<1024>("small_alloc_slow rsize={} slab size={}", rsize, slab_size);
#endif

      auto [slab, meta] = Config::Backend::alloc_chunk(
        get_backend_local_state(),
        slab_size,
        PagemapEntry::encode(
          public_state(), sizeclass_t::from_small_class(sizeclass)));

      if (slab == nullptr)
      {
        return nullptr;
      }

      // Set meta slab to empty.
      meta->initialise(
        sizeclass, address_cast(slab), entropy.get_free_list_key());

      // Build a free list for the slab
      alloc_new_list(slab, meta, rsize, slab_size, entropy);

      auto domesticate =
        [this](freelist::QueuePtr p) SNMALLOC_FAST_PATH_LAMBDA {
          return capptr_domesticate<Config>(backend_state_ptr(), p);
        };
      auto [p, still_active] = BackendSlabMetadata::alloc_free_list(
        domesticate, meta, fast_free_list, entropy, sizeclass);

      if (still_active)
      {
        alloc_classes[sizeclass].length++;
        alloc_classes[sizeclass].available.insert(meta);
      }
      else
      {
        laden.insert(meta);
      }

      auto r = finish_alloc<zero_mem, Config>(p, sizeclass);
      return ticker.check_tick(r);
    }

    /**
     * Flush the cached state and delayed deallocations
     *
     * Returns true if messages are sent to other threads.
     */
    bool flush(bool destroy_queue = false)
    {
      SNMALLOC_ASSERT(attached_cache != nullptr);
      auto local_state = backend_state_ptr();
      auto domesticate = [local_state](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<Config>(local_state, p);
                           };

      if (destroy_queue)
      {
        auto p_wild = message_queue().destroy();
        auto p_tame = domesticate(p_wild);

        while (p_tame != nullptr)
        {
          bool need_post = true; // Always going to post, so ignore.
          auto n_tame =
            p_tame->atomic_read_next(RemoteAllocator::key_global, domesticate);
          const PagemapEntry& entry =
            Config::Backend::get_metaentry(snmalloc::address_cast(p_tame));
          handle_dealloc_remote(entry, p_tame.as_void(), need_post);
          p_tame = n_tame;
        }
      }
      else
      {
        // Process incoming message queue
        // Loop as normally only processes a batch
        while (has_messages())
          handle_message_queue([]() {});
      }

      auto posted = attached_cache->flush<sizeof(CoreAllocator), Config>(
        backend_state_ptr(),
        [&](capptr::Alloc<void> p) { dealloc_local_object(p); });

      // We may now have unused slabs, return to the global allocator.
      for (smallsizeclass_t sizeclass = 0; sizeclass < NUM_SMALL_SIZECLASSES;
           sizeclass++)
      {
        dealloc_local_slabs<true>(sizeclass);
      }

      laden.iterate([this, domesticate](
                      BackendSlabMetadata* meta) SNMALLOC_FAST_PATH_LAMBDA {
        if (!meta->is_large())
        {
          meta->free_queue.validate(entropy.get_free_list_key(), domesticate);
        }
      });

      return posted;
    }

    // This allows the caching layer to be attached to an underlying
    // allocator instance.
    void attach(LocalCache* c)
    {
#ifdef SNMALLOC_TRACING
      message<1024>("Attach cache to {}", this);
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
      auto& key = entropy.get_free_list_key();

      auto error = [&result, &key](auto slab_metadata) {
        auto slab_interior = slab_metadata->get_slab_interior(key);
        const PagemapEntry& entry =
          Config::Backend::get_metaentry(slab_interior);
        SNMALLOC_ASSERT(slab_metadata == entry.get_slab_metadata());
        auto size_class = entry.get_sizeclass();
        auto slab_size = sizeclass_full_to_slab_size(size_class);
        auto slab_start = bits::align_down(slab_interior, slab_size);

        if (result != nullptr)
          *result = false;
        else
          report_fatal_error(
            "debug_is_empty: found non-empty allocator: size={} on "
            "slab_start {}",
            sizeclass_full_to_size(size_class),
            slab_start);
      };

      auto test = [&error](auto& queue) {
        queue.iterate([&error](auto slab_metadata) {
          if (slab_metadata->needed() != 0)
          {
            error(slab_metadata);
          }
        });
      };

      bool sent_something = flush(true);

      for (auto& alloc_class : alloc_classes)
      {
        test(alloc_class.available);
      }

      if (!laden.is_empty())
      {
        error(laden.peek());
      }

      // Place the static stub message on the queue.
      init_message_queue();

#ifdef SNMALLOC_TRACING
      message<1024>("debug_is_empty - done");
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
      message<1024>("debug_is_empty");
#endif
      if (attached_cache == nullptr)
      {
        // We need a cache to perform some operations, so set one up
        // temporarily
        LocalCache temp(public_state());
        attach(&temp);
#ifdef SNMALLOC_TRACING
        message<1024>("debug_is_empty - attach a cache");
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

  template<typename Config>
  class ConstructCoreAlloc
  {
    using CA = CoreAllocator<Config>;

  public:
    static capptr::Alloc<CA> make()
    {
      size_t size = sizeof(CA);
      size_t round_sizeof = Aal::capptr_size_round(size);
      size_t request_size = bits::next_pow2(round_sizeof);
      size_t spare = request_size - round_sizeof;

      auto raw =
        Config::Backend::template alloc_meta_data<CA>(nullptr, request_size);

      if (raw == nullptr)
      {
        Config::Pal::error("Failed to initialise thread local allocator.");
      }

      capptr::Alloc<void> spare_start = pointer_offset(raw, round_sizeof);
      Range<capptr::bounds::Alloc> r{spare_start, spare};

      auto p = capptr::Alloc<CA>::unsafe_from(new (raw.unsafe_ptr()) CA(r));

      // Remove excess from the bounds.
      p = Aal::capptr_bound<CA, capptr::bounds::Alloc>(p, round_sizeof);
      return p;
    }
  };

  /**
   * Use this alias to access the pool of allocators throughout snmalloc.
   */
  template<typename Config>
  using AllocPool =
    Pool<CoreAllocator<Config>, ConstructCoreAlloc<Config>, Config::pool>;
} // namespace snmalloc
