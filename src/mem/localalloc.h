#pragma once

#ifdef _MSC_VER
#  define ALLOCATOR __declspec(allocator)
#else
#  define ALLOCATOR
#endif

#include "../ds/ptrwrap.h"
#include "corealloc.h"
#include "freelist.h"
#include "localcache.h"
#include "pool.h"
#include "remotecache.h"
#include "sizeclasstable.h"

#ifdef SNMALLOC_PASS_THROUGH
#  include "external_alloc.h"
#endif

#ifdef SNMALLOC_TRACING
#  include <iostream>
#endif
#include <string.h>
#include <utility>
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
   * A local allocator contains the fast-path allocation routines and
   * encapsulates all of the behaviour of an allocator that is local to some
   * context, typically a thread.  This delegates to a `CoreAllocator` for all
   * slow-path operations, including anything that requires claiming new chunks
   * of address space.
   *
   * The template parameter defines the configuration of this allocator and is
   * passed through to the associated `CoreAllocator`.  The `Options` structure
   * of this defines one property that directly affects the behaviour of the
   * local allocator: `LocalAllocSupportsLazyInit`, which defaults to true,
   * defines whether the local allocator supports lazy initialisation.  If this
   * is true then the local allocator will construct a core allocator the first
   * time it needs to perform a slow-path operation.  If this is false then the
   * core allocator must be provided externally by invoking the `init` method
   * on this class *before* any allocation-related methods are called.
   */
  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
  class LocalAllocator
  {
  public:
    using StateHandle = SharedStateHandle;

  private:
    using CoreAlloc = CoreAllocator<SharedStateHandle>;

    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    // Also contains remote deallocation cache.
    LocalCache local_cache{&SharedStateHandle::unused_remote};

    // Underlying allocator for most non-fast path operations.
    CoreAlloc* core_alloc{nullptr};

    // As allocation and deallocation can occur during thread teardown
    // we need to record if we are already in that state as we will not
    // receive another teardown call, so each operation needs to release
    // the underlying data structures after the call.
    bool post_teardown{false};

    /**
     * Checks if the core allocator has been initialised, and runs the
     * `action` with the arguments, args.
     *
     * If the core allocator is not initialised, then first initialise it,
     * and then perform the action using the core allocator.
     *
     * This is an abstraction of the common pattern of check initialisation,
     * and then performing the operations.  It is carefully crafted to tail
     * call the continuations, and thus generate good code for the fast path.
     */
    template<typename Action, typename... Args>
    SNMALLOC_FAST_PATH decltype(auto) check_init(Action action, Args... args)
    {
      if (SNMALLOC_LIKELY(core_alloc != nullptr))
      {
        return core_alloc->handle_message_queue(action, core_alloc, args...);
      }
      return lazy_init(action, args...);
    }

    /**
     * This initialises the fast allocator by acquiring a core allocator, and
     * setting up its local copy of data structures.
     *
     * If the allocator does not support lazy initialisation then this assumes
     * that initialisation has already taken place and invokes the action
     * immediately.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto) lazy_init(Action action, Args... args)
    {
      SNMALLOC_ASSERT(core_alloc == nullptr);
      if constexpr (!SharedStateHandle::Options.LocalAllocSupportsLazyInit)
      {
        SNMALLOC_CHECK(
          false &&
          "lazy_init called on an allocator that doesn't support lazy "
          "initialisation");
        // Unreachable, but needed to keep the type checker happy in deducing
        // the return type of this function.
        return static_cast<decltype(action(core_alloc, args...))>(nullptr);
      }
      else
      {
        // Initialise the thread local allocator
        if constexpr (SharedStateHandle::Options.CoreAllocOwnsLocalState)
        {
          init();
        }

        // register_clean_up must be called after init.  register clean up may
        // be implemented with allocation, so need to ensure we have a valid
        // allocator at this point.
        if (!post_teardown)
          // Must be called at least once per thread.
          // A pthread implementation only calls the thread destruction handle
          // if the key has been set.
          SharedStateHandle::register_clean_up();

        // Perform underlying operation
        auto r = action(core_alloc, args...);

        // After performing underlying operation, in the case of teardown
        // already having begun, we must flush any state we just acquired.
        if (post_teardown)
        {
#ifdef SNMALLOC_TRACING
          std::cout << "post_teardown flush()" << std::endl;
#endif
          // We didn't have an allocator because the thread is being torndown.
          // We need to return any local state, so we don't leak it.
          flush();
        }

        return r;
      }
    }

    /**
     * Allocation that are larger than are handled by the fast allocator must be
     * passed to the core allocator.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH capptr::Alloc<void> alloc_not_small(size_t size)
    {
      if (size == 0)
      {
        // Deal with alloc zero of with a small object here.
        // Alternative semantics giving nullptr is also allowed by the
        // standard.
        return small_alloc<NoZero>(1);
      }

      return check_init([&](CoreAlloc* core_alloc) {
        // Grab slab of correct size
        // Set remote as large allocator remote.
        auto [chunk, meta] = ChunkAllocator::alloc_chunk<SharedStateHandle>(
          core_alloc->get_backend_local_state(),
          core_alloc->chunk_local_state,
          size_to_sizeclass_full(size),
          large_size_to_chunk_sizeclass(size),
          large_size_to_chunk_size(size),
          SharedStateHandle::fake_large_remote);
        // set up meta data so sizeclass is correct, and hence alloc size, and
        // external pointer.
#ifdef SNMALLOC_TRACING
        std::cout << "size " << size << " pow2 size "
                  << bits::next_pow2_bits(size) << std::endl;
#endif

        // Note that meta data is not currently used for large allocs.
        //        meta->initialise(size_to_sizeclass(size));
        UNUSED(meta);

        if (zero_mem == YesZero)
        {
          SharedStateHandle::Pal::template zero<false>(
            chunk.unsafe_ptr(), size);
        }

        return capptr_chunk_is_alloc(capptr_to_user_address_control(chunk));
      });
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH capptr::Alloc<void> small_alloc(size_t size)
    {
      auto domesticate = [this](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<SharedStateHandle>(
                               core_alloc->backend_state_ptr(), p);
                           };
      auto slowpath = [&](
                        smallsizeclass_t sizeclass,
                        freelist::Iter<>* fl) SNMALLOC_FAST_PATH_LAMBDA {
        if (SNMALLOC_LIKELY(core_alloc != nullptr))
        {
          return core_alloc->handle_message_queue(
            [](
              CoreAlloc* core_alloc,
              smallsizeclass_t sizeclass,
              freelist::Iter<>* fl) {
              return core_alloc->template small_alloc<zero_mem>(sizeclass, *fl);
            },
            core_alloc,
            sizeclass,
            fl);
        }
        return lazy_init(
          [&](CoreAlloc*, smallsizeclass_t sizeclass) {
            return small_alloc<zero_mem>(sizeclass_to_size(sizeclass));
          },
          sizeclass);
      };

      return local_cache.template alloc<zero_mem, SharedStateHandle>(
        domesticate, size, slowpath);
    }

    /**
     * Send all remote deallocation to other threads.
     */
    void post_remote_cache()
    {
      core_alloc->post();
    }

    /**
     * Slow path for deallocation we do not have space for this remote
     * deallocation. This could be because,
     *   - we actually don't have space for this remote deallocation,
     *     and need to send them on; or
     *   - the allocator was not already initialised.
     * In the second case we need to recheck if this is a remote deallocation,
     * as we might acquire the originating allocator.
     */
    SNMALLOC_SLOW_PATH void dealloc_remote_slow(capptr::Alloc<void> p)
    {
      if (core_alloc != nullptr)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Remote dealloc post" << p.unsafe_ptr() << " size "
                  << alloc_size(p.unsafe_ptr()) << std::endl;
#endif
        MetaEntry entry = SharedStateHandle::Pagemap::get_metaentry(
          core_alloc->backend_state_ptr(), address_cast(p));
        local_cache.remote_dealloc_cache.template dealloc<sizeof(CoreAlloc)>(
          entry.get_remote()->trunc_id(), p, key_global);
        post_remote_cache();
        return;
      }

      // Recheck what kind of dealloc we should do in case the allocator we get
      // from lazy_init is the originating allocator.  (TODO: but note that this
      // can't suddenly become a large deallocation; the only distinction is
      // between being ours to handle and something to post to a Remote.)
      lazy_init(
        [&](CoreAlloc*, CapPtr<void, capptr::bounds::Alloc> p) {
          dealloc(p.unsafe_ptr()); // TODO don't double count statistics
          return nullptr;
        },
        p);
    }

    /**
     * Abstracts access to the message queue to handle different
     * layout configurations of the allocator.
     */
    auto& message_queue()
    {
      return local_cache.remote_allocator;
    }

    /**
     * Call `SharedStateHandle::is_initialised()` if it is implemented,
     * unconditionally returns true otherwise.
     */
    SNMALLOC_FAST_PATH
    bool is_initialised()
    {
      return call_is_initialised<SharedStateHandle>(nullptr, 0);
    }

    /**
     * SFINAE helper.  Matched only if `T` implements `ensure_init`.  Calls it
     * if it exists.
     */
    template<typename T>
    SNMALLOC_FAST_PATH auto call_ensure_init(T*, int)
      -> decltype(T::ensure_init())
    {
      T::ensure_init();
    }

    /**
     * SFINAE helper.  Matched only if `T` does not implement `ensure_init`.
     * Does nothing if called.
     */
    template<typename T>
    SNMALLOC_FAST_PATH auto call_ensure_init(T*, long)
    {}

    /**
     * Call `SharedStateHandle::ensure_init()` if it is implemented, do
     * nothing otherwise.
     */
    SNMALLOC_FAST_PATH
    void ensure_init()
    {
      call_ensure_init<SharedStateHandle>(nullptr, 0);
    }

  public:
    constexpr LocalAllocator() = default;
    LocalAllocator(const LocalAllocator&) = delete;
    LocalAllocator& operator=(const LocalAllocator&) = delete;

    /**
     * Initialise the allocator.  For allocators that support local
     * initialisation, this is called with a core allocator that this class
     * allocates (from a pool allocator) the first time it encounters a slow
     * path.  If this class is configured without lazy initialisation support
     * then this must be called externally
     */
    void init(CoreAlloc* c)
    {
      // Initialise the global allocator structures
      ensure_init();

      // Should only be called if the allocator has not been initialised.
      SNMALLOC_ASSERT(core_alloc == nullptr);

      // Attach to it.
      c->attach(&local_cache);
      core_alloc = c;
#ifdef SNMALLOC_TRACING
      std::cout << "init(): core_alloc=" << core_alloc << "@" << &local_cache
                << std::endl;
#endif
      // local_cache.stats.sta rt();
    }

    // This is effectively the constructor for the LocalAllocator, but due to
    // not wanting initialisation checks on the fast path, it is initialised
    // lazily.
    void init()
    {
      // Initialise the global allocator structures
      ensure_init();
      // Grab an allocator for this thread.
      init(AllocPool<SharedStateHandle>::acquire(&(this->local_cache)));
    }

    // Return all state in the fast allocator and release the underlying
    // core allocator.  This is used during teardown to empty the thread
    // local state.
    void flush()
    {
      // Detached thread local state from allocator.
      if (core_alloc != nullptr)
      {
        core_alloc->flush();

        // core_alloc->stats().add(local_cache.stats);
        // // Reset stats, required to deal with repeated flushing.
        // new (&local_cache.stats) Stats();

        // Detach underlying allocator
        core_alloc->attached_cache = nullptr;
        // Return underlying allocator to the system.
        if constexpr (SharedStateHandle::Options.CoreAllocOwnsLocalState)
        {
          AllocPool<SharedStateHandle>::release(core_alloc);
        }

        // Set up thread local allocator to look like
        // it is new to hit slow paths.
        core_alloc = nullptr;
#ifdef SNMALLOC_TRACING
        std::cout << "flush(): core_alloc=" << core_alloc << std::endl;
#endif
        local_cache.remote_allocator = &SharedStateHandle::unused_remote;
        local_cache.remote_dealloc_cache.capacity = 0;
      }
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
      if (SNMALLOC_LIKELY(
            (size - 1) <= (sizeclass_to_size(NUM_SMALL_SIZECLASSES - 1) - 1)))
      {
        // Small allocations are more likely. Improve
        // branch prediction by placing this case first.
        return capptr_reveal(small_alloc<zero_mem>(size));
      }

      return capptr_reveal(alloc_not_small<zero_mem>(size));
#endif
    }

    /**
     * Allocate memory of a statically known size.
     */
    template<size_t size, ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc()
    {
      return alloc<zero_mem>(size);
    }

    SNMALLOC_FAST_PATH void dealloc(void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      external_alloc::free(p_raw);
#else
      // Care is needed so that dealloc(nullptr) works before init
      //  The backend allocator must ensure that a minimal page map exists
      //  before init, that maps null to a remote_deallocator that will never
      //  be in thread local state.

      capptr::AllocWild<void> p_wild = capptr_from_client(p_raw);

      /*
       * p_tame may be nullptr, even if p_raw/p_wild are not, in the case
       * where domestication fails.  We exclusively use p_tame below so that
       * such failures become no ops; in the nullptr path, which should be
       * well off the fast path, we could be slightly more aggressive and test
       * that p_raw is also nullptr and Pal::error() if not. (TODO)
       *
       * We do not rely on the bounds-checking ability of domestication here,
       * and just check the address (and, on other architectures, perhaps
       * well-formedness) of this pointer.  The remainder of the logic will
       * deal with the object's extent.
       */
      capptr::Alloc<void> p_tame = capptr_domesticate<SharedStateHandle>(
        core_alloc->backend_state_ptr(), p_wild);

      const MetaEntry& entry = SharedStateHandle::Pagemap::get_metaentry(
        core_alloc->backend_state_ptr(), address_cast(p_tame));
      if (SNMALLOC_LIKELY(local_cache.remote_allocator == entry.get_remote()))
      {
        if (SNMALLOC_LIKELY(CoreAlloc::dealloc_local_object_fast(
              entry, p_tame, local_cache.entropy)))
          return;
        core_alloc->dealloc_local_object_slow(entry);
        return;
      }

      if (SNMALLOC_LIKELY(entry.get_remote() != SharedStateHandle::fake_large_remote))
      {
        // Check if we have space for the remote deallocation
        if (local_cache.remote_dealloc_cache.reserve_space(entry))
        {
          local_cache.remote_dealloc_cache.template dealloc<sizeof(CoreAlloc)>(
            entry.get_remote()->trunc_id(), p_tame, key_global);
#  ifdef SNMALLOC_TRACING
          std::cout << "Remote dealloc fast" << p_raw << " size "
                    << alloc_size(p_raw) << std::endl;
#  endif
          return;
        }

        dealloc_remote_slow(p_tame);
        return;
      }

      // Large deallocation or null.
      // also checks for managed by page map.
      if (SNMALLOC_LIKELY((p_tame != nullptr) && !entry.get_sizeclass().is_default()))
      {
        size_t entry_sizeclass = entry.get_sizeclass().as_large();

        size_t size = bits::one_at_bit(entry_sizeclass);
        size_t slab_sizeclass =
          metaentry_chunk_sizeclass_to_slab_sizeclass(entry_sizeclass);

        // Check for start of allocation.
        check_client(
          pointer_align_down(p_tame, size) == p_tame,
          "Not start of an allocation.");

#  ifdef SNMALLOC_TRACING
        std::cout << "Large deallocation: " << size
                  << " chunk sizeclass: " << slab_sizeclass << std::endl;
#  else
        UNUSED(size);
#  endif

        auto slab_record =
          static_cast<ChunkRecord*>(entry.get_metaslab_no_remote());

        SNMALLOC_ASSERT(
          address_cast(slab_record->meta_common.chunk) == address_cast(p_tame));

        check_init(
          [](
            CoreAlloc* core_alloc,
            ChunkRecord* slab_record,
            size_t slab_sizeclass) {
            ChunkAllocator::dealloc<SharedStateHandle>(
              core_alloc->get_backend_local_state(),
              core_alloc->chunk_local_state,
              slab_record,
              slab_sizeclass);
            return nullptr;
          },
          slab_record,
          slab_sizeclass);
        return;
      }

      // If p_tame is not null, then dealloc has been call on something
      // it shouldn't be called on.
      // TODO: Should this be tested even in the !CHECK_CLIENT case?
      check_client(p_tame == nullptr, "Not allocated by snmalloc.");

#  ifdef SNMALLOC_TRACING
      std::cout << "nullptr deallocation" << std::endl;
#  endif
      return;
#endif
    }

    SNMALLOC_FAST_PATH void dealloc(void* p, size_t s)
    {
      UNUSED(s);
      dealloc(p);
    }

    template<size_t size>
    SNMALLOC_FAST_PATH void dealloc(void* p)
    {
      UNUSED(size);
      dealloc(p);
    }

    void teardown()
    {
#ifdef SNMALLOC_TRACING
      std::cout << "Teardown: core_alloc=" << core_alloc << "@" << &local_cache
                << std::endl;
#endif
      post_teardown = true;
      if (core_alloc != nullptr)
      {
        flush();
      }
    }

    SNMALLOC_FAST_PATH size_t alloc_size(const void* p_raw)
    {
#ifdef SNMALLOC_PASS_THROUGH
      return external_alloc::malloc_usable_size(const_cast<void*>(p_raw));
#else
      // TODO What's the domestication policy here?  At the moment we just
      // probe the pagemap with the raw address, without checks.  There could
      // be implicit domestication through the `SharedStateHandle::Pagemap` or
      // we could just leave well enough alone.

      // Note that alloc_size should return 0 for nullptr.
      // Other than nullptr, we know the system will be initialised as it must
      // be called with something we have already allocated.
      //
      // To handle this case we require the uninitialised pagemap contain an
      // entry for the first chunk of memory, that states it represents a
      // large object, so we can pull the check for null off the fast path.
      MetaEntry entry = SharedStateHandle::Pagemap::get_metaentry(
        core_alloc->backend_state_ptr(), address_cast(p_raw));

      return sizeclass_full_to_size(entry.get_sizeclass());
#endif
    }

    /**
     * Returns the Start/End of an object allocated by this allocator
     *
     * It is valid to pass any pointer, if the object was not allocated
     * by this allocator, then it give the start and end as the whole of
     * the potential pointer space.
     */
    template<Boundary location = Start>
    void* external_pointer(void* p)
    {
      // Note that each case uses `pointer_offset`, so that on
      // CHERI it is monotone with respect to the capability.
      // Note that the returned pointer could be outside the CHERI
      // bounds of `p`, and thus not something that can be followed.
      if constexpr (location == Start)
      {
        size_t index = index_in_object(p);
        return pointer_offset(p, 0 - index);
      }
      else if constexpr (location == End)
      {
        return pointer_offset(p, remaining_bytes(p) - 1);
      }
      else
      {
        return pointer_offset(p, remaining_bytes(p));
      }
    }

    /**
     * Returns the number of remaining bytes in an object.
     *
     * auto p = (char*)malloc(size)
     * remaining_bytes(p + n) == size - n     provided n < size
     */
    size_t remaining_bytes(const void* p)
    {
#ifndef SNMALLOC_PASS_THROUGH
      MetaEntry entry =
        SharedStateHandle::Pagemap::template get_metaentry<true>(
          core_alloc->backend_state_ptr(), address_cast(p));

      auto sizeclass = entry.get_sizeclass();
      return snmalloc::remaining_bytes(sizeclass, address_cast(p));
#else
      return pointer_diff(p, reinterpret_cast<void*>(UINTPTR_MAX));
#endif
    }

    /**
     * Returns the byte offset into an object.
     *
     * auto p = (char*)malloc(size)
     * index_in_object(p + n) == n     provided n < size
     */
    size_t index_in_object(const void* p)
    {
#ifndef SNMALLOC_PASS_THROUGH
      MetaEntry entry =
        SharedStateHandle::Pagemap::template get_metaentry<true>(
          core_alloc->backend_state_ptr(), address_cast(p));

      auto sizeclass = entry.get_sizeclass();
      return snmalloc::index_in_object(sizeclass, address_cast(p));
#else
      return reinterpret_cast<size_t>(p);
#endif
    }

    /**
     * Accessor, returns the local cache.  If embedding code is allocating the
     * core allocator for use by this local allocator then it needs to access
     * this field.
     */
    LocalCache& get_local_cache()
    {
      return local_cache;
    }
  };
} // namespace snmalloc
