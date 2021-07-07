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

  // This class contains the fastest path code for the allocator.
  template<class SharedStateHandle>
  class LocalAllocator
  {
    using CoreAlloc = CoreAlloc<SharedStateHandle>;

  private:
    /**
     * Contains a way to access all the shared state for this allocator.
     * This may have no dynamic state, and be purely static.
     */
    SharedStateHandle handle;

    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    // Also contains remote deallocation cache.
    LocalCache local_cache;

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
      if (likely(core_alloc != nullptr))
      {
        return core_alloc->handle_message_queue(action, core_alloc, args...);
      }
      return lazy_init(action, args...);
    }

    /**
     * This initialises the fast allocator by acquiring a core allocator, and
     * setting up its local copy of data structures.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto) lazy_init(Action action, Args... args)
    {
      SNMALLOC_ASSERT(core_alloc == nullptr);

      // Initialise the thread local allocator
      init();

      // register_clean_up must be called after init.  register clean up may be
      // implemented with allocation, so need to ensure we have a valid
      // allocator at this point.
      if (!post_teardown)
        // Must be called at least once per thread.
        // A pthread implementation only calls the thread destruction handle
        // if the key has been set.
        handle.register_clean_up();

      // Perform underlying operation
      auto r = action(core_alloc, args...);

      // After performing underlying operation, in the case of teardown already
      // having begun, we must flush any state we just acquired.
      if (post_teardown)
      {
        // We didn't have an allocator because the thread is being torndown.
        // We need to return any local state, so we don't leak it.
        flush();
      }

      return r;
    }

    /**
     * Allocation that are larger than are handled by the fast allocator must be
     * passed to the core allocator.
     */
    template<ZeroMem zero_mem>
    SNMALLOC_SLOW_PATH void* alloc_not_small(size_t size)
    {
      if (size == 0)
      {
        // Deal with alloc zero giving nullptr.
        // Alternative semantics of a small object here is also allowed by the
        // standard.
        return nullptr;
      }

      return check_init([&](CoreAlloc* core_alloc) {
        // Grab slab of correct size
        // Set remote as large allocator remote.
        auto [chunk, meta] = ChunkAllocator::alloc_chunk(
          handle,
          core_alloc->backend_state,
          bits::next_pow2_bits(size), // TODO
          large_size_to_chunk_sizeclass(size),
          large_size_to_chunk_size(size),
          handle.fake_large_remote);
        // set up meta data so sizeclass is correct, and hence alloc size, and
        // external pointer.
#ifdef SNMALLOC_TRACING
        std::cout << "size " << size << " sizeclass " << size_to_sizeclass(size)
                  << std::endl;
#endif

        // Note that meta data is not currently used for large allocs.
        //        meta->initialise(size_to_sizeclass(size));

        if (zero_mem == YesZero)
        {
          SharedStateHandle::Pal::template zero<false>(
            chunk.unsafe_ptr(), size);
        }

        return chunk.unsafe_ptr();
      });
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH void* small_alloc(size_t size)
    {
      //      SNMALLOC_ASSUME(size <= sizeclass_to_size(NUM_SIZECLASSES));
      auto slowpath = [&](
                        sizeclass_t sizeclass,
                        FreeListIter* fl) SNMALLOC_FAST_PATH {
        if (likely(core_alloc != nullptr))
        {
          return core_alloc->handle_message_queue(
            [](CoreAlloc* core_alloc, sizeclass_t sizeclass, FreeListIter* fl) {
              return core_alloc->template small_alloc<zero_mem>(sizeclass, *fl);
            },
            core_alloc,
            sizeclass,
            fl);
        }
        return lazy_init(
          [&](CoreAlloc*, sizeclass_t sizeclass) {
            return small_alloc<zero_mem>(sizeclass_to_size(sizeclass));
          },
          sizeclass);
      };

      return local_cache.template alloc<zero_mem, SharedStateHandle>(
        size, slowpath);
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
    SNMALLOC_SLOW_PATH void dealloc_remote_slow(void* p)
    {
      if (core_alloc != nullptr)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Remote dealloc post" << p << " size " << alloc_size(p)
                  << std::endl;
#endif
        MetaEntry entry = SharedStateHandle::Backend::get_meta_data(
          handle.get_backend_state(), address_cast(p));
        local_cache.remote_dealloc_cache.template dealloc<sizeof(CoreAlloc)>(
          entry.get_remote()->trunc_id(), CapPtr<void, CBAlloc>(p));
        post_remote_cache();
        return;
      }

      // Recheck what kind of dealloc we should do incase, the allocator we get
      // from lazy_init is the originating allocator.
      lazy_init(
        [&](CoreAlloc*, void* p) {
          dealloc(p); // TODO don't double count statistics
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
      return local_cache.remote_allocator->message_queue;
    }

  public:
    constexpr LocalAllocator()
    : handle(SharedStateHandle::get_handle()),
      local_cache(&handle.unused_remote)
    {}

    LocalAllocator(SharedStateHandle handle)
    : handle(handle), local_cache(&handle.unused_remote)
    {}

    // This is effectively the constructor for the LocalAllocator, but due to
    // not wanting initialisation checks on the fast path, it is initialised
    // lazily.
    void init()
    {
      // Initialise the global allocator structures
      handle.ensure_init();

      // Should only be called if the allocator has not been initialised.
      SNMALLOC_ASSERT(core_alloc == nullptr);

      // Grab an allocator for this thread.
      auto c = Pool<CoreAlloc>::acquire(handle, &(this->local_cache), handle);

      // Attach to it.
      c->attach(&local_cache);
      core_alloc = c;

      // local_cache.stats.start();
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
        Pool<CoreAlloc>::release(handle, core_alloc);

        // Set up thread local allocator to look like
        // it is new to hit slow paths.
        core_alloc = nullptr;
        local_cache.remote_allocator = &handle.unused_remote;
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
      if (likely((size - 1) <= (sizeclass_to_size(NUM_SIZECLASSES - 1) - 1)))
      {
        // Small allocations are more likely. Improve
        // branch prediction by placing this case first.
        return small_alloc<zero_mem>(size);
      }

      // TODO capptr_reveal?
      return alloc_not_small<zero_mem>(size);
#endif
    }

    /**
     * Allocate memory of a statically known size.
     */
    template<size_t size, ZeroMem zero_mem = NoZero>
    SNMALLOC_FAST_PATH ALLOCATOR void* alloc()
    {
      // TODO optimise
      return alloc<zero_mem>(size);
    }

    SNMALLOC_FAST_PATH void dealloc(void* p)
    {
      // TODO Pass through code!
      // TODO:
      // Care is needed so that dealloc(nullptr) works before init
      //  The backend allocator must ensure that a minimal page map exists
      //  before init, that maps null to a remote_deallocator that will never be
      //  in thread local state.

      const MetaEntry& entry = SharedStateHandle::Backend::get_meta_data(
        handle.get_backend_state(), address_cast(p));
      if (likely(local_cache.remote_allocator == entry.get_remote()))
      {
        if (likely(CoreAlloc::dealloc_local_object_fast(
              entry, p, local_cache.entropy)))
          return;
        core_alloc->dealloc_local_object_slow(entry);
        return;
      }

      if (likely(entry.get_remote() != handle.fake_large_remote))
      {
        // Check if we have space for the remote deallocation
        if (local_cache.remote_dealloc_cache.reserve_space(entry))
        {
          local_cache.remote_dealloc_cache.template dealloc<sizeof(CoreAlloc)>(
            entry.get_remote()->trunc_id(), CapPtr<void, CBAlloc>(p));
#ifdef SNMALLOC_TRACING
          std::cout << "Remote dealloc fast" << p << " size " << alloc_size(p)
                    << std::endl;
#endif
          return;
        }

        dealloc_remote_slow(p);
        return;
      }

      // Large deallocation or null.
      if (likely(p != nullptr))
      {
        // Check this is managed by this pagemap.
        check_client(entry.get_sizeclass() != 0, "Not allocated by snmalloc.");

        size_t size = bits::one_at_bit(entry.get_sizeclass());

        // Check for start of allocation.
        check_client(
          pointer_align_down(p, size) == p, "Not start of an allocation.");

        size_t slab_sizeclass = large_size_to_chunk_sizeclass(size);
#ifdef SNMALLOC_TRACING
        std::cout << "Large deallocation: " << size
                  << " chunk sizeclass: " << slab_sizeclass << std::endl;
#endif
        ChunkRecord* slab_record =
          reinterpret_cast<ChunkRecord*>(entry.get_metaslab());
        slab_record->chunk = CapPtr<void, CBChunk>(p);
        ChunkAllocator::dealloc(handle, slab_record, slab_sizeclass);
        return;
      }

#ifdef SNMALLOC_TRACING
      std::cout << "nullptr deallocation" << std::endl;
#endif
      return;
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
      std::cout << "Teardown" << std::endl;
#endif
      post_teardown = true;
      if (core_alloc != nullptr)
      {
        flush();
      }
    }

    SNMALLOC_FAST_PATH size_t alloc_size(const void* p_raw)
    {
      // Note that this should return 0 for nullptr.
      // Other than nullptr, we know the system will be initialised as it must
      // be called with something we have already allocated.
      // To handle this case we require the uninitialised pagemap contain an
      // entry for the first chunk of memory, that states it represents a large
      // object, so we can pull the check for null off the fast path.
      MetaEntry entry = SharedStateHandle::Backend::get_meta_data(
        handle.get_backend_state(), address_cast(p_raw));

      if (likely(entry.get_remote() != handle.fake_large_remote))
        return sizeclass_to_size(entry.get_sizeclass());

      // Sizeclass zero is for large is actually zero
      if (likely(entry.get_sizeclass() != 0))
        return bits::one_at_bit(entry.get_sizeclass());

      return 0;
    }

    /**
     * Returns the Start/End of an object allocated by this allocator
     *
     * It is valid to pass any pointer, if the object was not allocated
     * by this allocator, then it give the start and end as the whole of
     * the potential pointer space.
     */
    template<Boundary location = Start>
    void* external_pointer(void* p_raw)
    {
      // TODO bring back the CHERI bits. Wes to review if required.
      if (likely(handle.is_initialised()))
      {
        MetaEntry entry =
          SharedStateHandle::Backend::template get_meta_data<true>(
            handle.get_backend_state(), address_cast(p_raw));
        auto sizeclass = entry.get_sizeclass();
        if (likely(entry.get_remote() != handle.fake_large_remote))
        {
          auto rsize = sizeclass_to_size(sizeclass);
          auto offset =
            address_cast(p_raw) & (sizeclass_to_slab_size(sizeclass) - 1);
          auto start_offset = round_by_sizeclass(sizeclass, offset);
          if constexpr (location == Start)
          {
            UNUSED(rsize);
            return pointer_offset(p_raw, start_offset - offset);
          }
          else if constexpr (location == End)
            return pointer_offset(p_raw, rsize + start_offset - offset - 1);
          else
            return pointer_offset(p_raw, rsize + start_offset - offset);
        }

        // Sizeclass zero of a large allocation is used for not managed by us.
        if (likely(sizeclass != 0))
        {
          // This is a large allocation, find start by masking.
          auto rsize = bits::one_at_bit(sizeclass);
          auto start = pointer_align_down(p_raw, rsize);
          if constexpr (location == Start)
            return start;
          else if constexpr (location == End)
            return pointer_offset(start, rsize);
          else
            return pointer_offset(start, rsize - 1);
        }
      }
      else
      {
        // Allocator not initialised, so definitely not our allocation
      }

      if constexpr ((location == End) || (location == OnePastEnd))
        // We don't know the End, so return MAX_PTR
        return pointer_offset<void, void>(nullptr, UINTPTR_MAX);
      else
        // We don't know the Start, so return MIN_PTR
        return nullptr;
    }
  };
} // namespace snmalloc