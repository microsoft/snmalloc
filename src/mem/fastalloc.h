#pragma once

#ifdef _MSC_VER
#  define ALLOCATOR __declspec(allocator)
#else
#  define ALLOCATOR
#endif

#include "../ds/ptrwrap.h"
#include "corealloc.h"
#include "fastcache.h"
#include "freelist.h"
#include "pool.h"
#include "remoteallocator.h"
#include "sizeclasstable.h"

#include <iostream>
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
  class FastAllocator
  {
    using CoreAlloc = CoreAlloc<SharedStateHandle>;

  private:
    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    FastCache small_cache;

    /**
     * The total amount of memory we are waiting for before we will dispatch
     * to other allocators. Zero means we have not initialised the allocator
     * yet. This is initialised to the 0 so that we always hit a slow path to
     * start with, when we hit the slow path and need to dispatch everything, we
     * can check if we are a real allocator and lazily provide a real allocator.
     */
    int64_t capacity{0};

    // Underlying allocator for most non-fast path operations.
    CoreAlloc* core_alloc{nullptr};

    // Pointer to the remote allocator message_queue, used to check
    // if a deallocation is local.
    RemoteAllocator* remote_allocator;

    // As allocation and deallocation can occur during thread teardown
    // we need to record if we are already in that state as we will not
    // receive another teardown call, so each operation needs to release
    // the underlying data structures after the call.
    bool post_teardown{false};

    /**
     * Contains a way to access all the shared state for this allocator.
     * This may have no dynamic state, and be purely static.
     */
    SharedStateHandle handle;

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
        return handle_message_queue(action, core_alloc, args...);
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

      Singleton<int, SharedStateHandle::init>::get();

      init();

      // register_clean_up must be called after init.  register clean up may be
      // implemented with allocation, so need to ensure we have a valid
      // allocator at this point.
      if (!post_teardown)
        // TODO: Should this be a singleton, so we only call it once?
        SharedStateHandle::register_clean_up();

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
        // Deal with alloc zero giving a small object.
        // Alternative semantics of nullptr here is also allowed by the
        // standard.
        return small_alloc<zero_mem>(1);
      }

      // TODO
      //  ?Do we need to initialise the allocator on this path?
      //   only if we are doing stats?

      // Grab slab of correct size
      // Set remote as large allocator remote.
      auto [slab, meta] = SlabAllocator::alloc(
        handle,
        large_size_to_slab_sizeclass(size),
        large_size_to_slab_size(size),
        handle.fake_large_remote);
      // set up meta data so sizeclass is correct, and hence alloc size, and
      // external pointer.
#ifdef SNMALLOC_TRACING
      std::cout << "size " << size << " sizeclass " << size_to_sizeclass(size)
                << std::endl;
#endif
      meta->initialise(size_to_sizeclass(size));

      if (zero_mem == YesZero)
      {
        SharedStateHandle::Pal::template zero<false>(slab.unsafe_capptr, size);
      }

      return slab.unsafe_capptr;
    }

    template<ZeroMem zero_mem>
    SNMALLOC_FAST_PATH void* small_alloc(size_t size)
    {
      // /      SNMALLOC_ASSUME(size <= SLAB_SIZE);
      auto slowpath = [&](
                        sizeclass_t sizeclass,
                        FreeListIter* fl) SNMALLOC_FAST_PATH {
        return check_init(
          //  Note:  FreeListIter& for fl would be nice, but codegen gets
          //  upset in clang.
          [&](CoreAlloc* core_alloc, sizeclass_t sizeclass, FreeListIter* fl) {
            // Setting up the message queue can cause a free list to be
            // populated, so need to check that initialisation hasn't caused
            // that.  Aggressive inlining will remove this.
            if (fl->empty())
              return core_alloc->template small_alloc<zero_mem>(sizeclass, *fl);

            auto r = capptr_reveal(
              capptr_export(fl->take(small_cache.entropy).as_void()));

            if (zero_mem == YesZero)
              SharedStateHandle::Pal::zero(r, sizeclass_to_size(sizeclass));

            return r;
          },
          sizeclass,
          fl);
      };

      return small_cache.template alloc<zero_mem, SharedStateHandle>(
        size, slowpath);
    }

    /**
     * Slow path for deallocation we do not have space for this remote
     * deallocation. This could be because,
     *   - we actually don't have space for this remote deallocation,
     *     and need to spend them on; or
     *   - the allocator was not already initialised.
     * In the second case we need to recheck if this is a remote deallocation,
     * as we might acquire the originating allocator.
     */
    SNMALLOC_SLOW_PATH void dealloc_slow(void* p)
    {
      if (core_alloc != nullptr)
      {
        //        handle_message_queue();

#ifdef SNMALLOC_TRACING
        std::cout << "Remote dealloc post" << p << " size " << alloc_size(p)
                  << std::endl;
#endif
        MetaEntry entry =
          BackendAllocator::get_meta_data(handle, address_cast(p));
        core_alloc->remote_cache.template dealloc<SharedStateHandle>(
          entry.remote->trunc_id(),
          CapPtr<void, CBAlloc>(p),
          entry.meta->sizeclass());
        core_alloc->remote_cache.post(handle, remote_allocator->trunc_id());
        capacity = REMOTE_CACHE;
        return;
      }

      // Recheck what kind of dealloc we should do incase, the allocator we get
      // from lazy_init is the originating allocator.
      lazy_init(
        [&](CoreAlloc*, void* p) {
          dealloc(p);
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
      return remote_allocator->message_queue;
    }

    /**
     * Check if this allocator has messages to deallocate blocks from another
     * thread
     */
    SNMALLOC_FAST_PATH bool has_messages()
    {
      return !(message_queue().is_empty());
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

    /**
     * Process remote frees into this allocator.
     */
    template<typename Action, typename... Args>
    SNMALLOC_SLOW_PATH decltype(auto)
    handle_message_queue_inner(Action action, Args... args)
    {
      for (size_t i = 0; i < REMOTE_BATCH; i++)
      {
        auto r = message_queue().dequeue();

        if (unlikely(!r.second))
          break;
#ifdef SNMALLOC_TRACING
        std::cout << "Handling remote" << std::endl;
#endif
        // Previously had special version of this code:
        //   handle_dealloc_remote(r.first);
        // TODO this needs to not double count stats
        // TODO this needs to not double revoke if using MTE
        // TODO batch post at the end of processing.
        // TODO thread capabilities?
        dealloc(r.first.unsafe_capptr);
      }

      return action(args...);
    }

  public:
    constexpr FastAllocator()
    {
      handle = SharedStateHandle::get_handle();
      remote_allocator = &handle.unused_remote;
    };

    // This is effectively the constructor for the FastAllocator, but due to
    // not wanting initialisation checks on the fast path, it is initialised
    // lazily.
    void init()
    {
      // Should only be called if the allocator has not been initialised.
      SNMALLOC_ASSERT(core_alloc == nullptr);

      // Grab an allocator for this thread.
      core_alloc =
        Pool<CoreAlloc>::acquire(handle, &(this->small_cache), handle);
      core_alloc->attached_cache = &(this->small_cache);

      small_cache.entropy = core_alloc->entropy;
      // small_cache.stats.start();

      remote_allocator = core_alloc->public_state();
    }

    // Return all state in the fast allocator and release the underlying
    // core allocator.  This is used during teardown to empty the thread
    // local state.
    void flush()
    {
      // Detached thread local state from allocator.
      if (core_alloc != nullptr)
      {
        small_cache.flush([&](auto p) { dealloc(p); });

        // core_alloc->stats().add(small_cache.stats);
        // // Reset stats, required to deal with repeated flushing.
        // new (&small_cache.stats) Stats();

        core_alloc->attached_cache = nullptr;
        // Return underlying allocator to the system.
        Pool<CoreAlloc>::release(handle, core_alloc);

        core_alloc = nullptr;
        remote_allocator = nullptr;
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
        // Allocations smaller than the slab size are more likely. Improve
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

      MetaEntry entry =
        BackendAllocator::get_meta_data(handle, address_cast(p));
      if (likely(remote_allocator == entry.remote))
      {
        if (core_alloc->dealloc_local_object(p))
          return;
        core_alloc->dealloc_local_object_slow(entry.meta, p);
        return;
      }

      if (likely(entry.remote != handle.fake_large_remote))
      {
        // Check if we have space for the remote deallocation
        if (likely(
              capacity > (int64_t)sizeclass_to_size(entry.meta->sizeclass())))
        {
          capacity -= sizeclass_to_size(entry.meta->sizeclass());
          core_alloc->remote_cache.template dealloc<SharedStateHandle>(
            entry.remote->trunc_id(),
            CapPtr<void, CBAlloc>(p),
            entry.meta->sizeclass());
#ifdef SNMALLOC_TRACING
          std::cout << "Remote dealloc fast" << p << " size " << alloc_size(p)
                    << std::endl;
#endif
          return;
        }

        dealloc_slow(p);
        return;
      }

      // Large deallocation or null.
      if (likely(p != nullptr))
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Large deallocation" << std::endl;
#endif
        // TODO Doesn't require local init! unless stats are on.
        // TODO check for start of allocation.
        size_t size = sizeclass_to_size(entry.meta->sizeclass());
        size_t sizeclass = large_size_to_slab_sizeclass(size);
        SlabRecord* slab_record = reinterpret_cast<SlabRecord*>(entry.meta);
        slab_record->slab = CapPtr<void, CBChunk>(p);
        SlabAllocator::dealloc(
          handle, slab_record, sizeclass_to_slab_sizeclass(sizeclass));
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

    SNMALLOC_SLOW_PATH size_t alloc_size(const void* p_raw)
    {
      // TODO nullptr, should return 0.
      // Other than nullptr, we know the system will be initialised as it must
      // be called with something we have already allocated.
      MetaEntry entry =
        BackendAllocator::get_meta_data(handle, address_cast(p_raw));
      return sizeclass_to_size(entry.meta->sizeclass());
    }

    template<Boundary location = Start>
    void* external_pointer(void* p_raw)
    {
      UNUSED(p_raw);
      return nullptr;
      // // TODO, can we optimise to not need initialisation?
      // return check_init(
      //   [](CoreAlloc* core_alloc, void* p_raw) {
      //     return core_alloc->template external_pointer<location>(p_raw);
      //   },
      //   p_raw);
    }
  };
} // namespace snmalloc