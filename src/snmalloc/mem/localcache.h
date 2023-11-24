#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "remotecache.h"
#include "sizeclasstable.h"

#include <string.h>

namespace snmalloc
{
  inline static SNMALLOC_FAST_PATH capptr::Alloc<void>
  finish_alloc_no_zero(freelist::HeadPtr p, smallsizeclass_t sizeclass)
  {
    SNMALLOC_ASSERT(is_start_of_object(
      sizeclass_t::from_small_class(sizeclass), address_cast(p)));
    UNUSED(sizeclass);

    return p.as_void();
  }

  template<ZeroMem zero_mem, typename Config>
  inline static SNMALLOC_FAST_PATH capptr::Alloc<void>
  finish_alloc(freelist::HeadPtr p, smallsizeclass_t sizeclass)
  {
    auto r = finish_alloc_no_zero(p, sizeclass);

    if constexpr (zero_mem == YesZero)
      Config::Pal::zero(r.unsafe_ptr(), sizeclass_to_size(sizeclass));

    // TODO: Should this be zeroing the free Object state, in the non-zeroing
    // case?

    return r;
  }

  // This is defined on its own, so that it can be embedded in the
  // thread local fast allocator, but also referenced from the
  // thread local core allocator.
  template<typename Config>
  struct LocalCache
  {
    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    freelist::Iter<> small_fast_free_lists[NUM_SMALL_SIZECLASSES] = {};

    // This is the entropy for a particular thread.
    LocalEntropy entropy;

    // Pointer to the remote allocator message_queue, used to check
    // if a deallocation is local.
    RemoteAllocator* remote_allocator;

    /**
     * Remote deallocations for other threads
     */
    RemoteDeallocCache<Config> remote_dealloc_cache;

    constexpr LocalCache(RemoteAllocator* remote_allocator)
    : remote_allocator(remote_allocator)
    {}

    /**
     * Return all the free lists to the allocator.  Used during thread teardown.
     */
    template<size_t allocator_size, typename DeallocFun>
    bool flush(typename Config::LocalState* local_state, DeallocFun dealloc)
    {
      auto& key = freelist::Object::key_root;
      auto domesticate = [local_state](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<Config>(local_state, p);
                           };

      for (size_t i = 0; i < NUM_SMALL_SIZECLASSES; i++)
      {
        // TODO could optimise this, to return the whole list in one append
        // call.
        while (!small_fast_free_lists[i].empty())
        {
          auto p = small_fast_free_lists[i].take(key, domesticate);
          SNMALLOC_ASSERT(is_start_of_object(
            sizeclass_t::from_small_class(i), address_cast(p)));
          dealloc(p.as_void());
        }
      }

      return remote_dealloc_cache.template post<allocator_size>(
        local_state, remote_allocator->trunc_id());
    }

    template<ZeroMem zero_mem, typename Slowpath, typename Domesticator>
    SNMALLOC_FAST_PATH capptr::Alloc<void>
    alloc(Domesticator domesticate, size_t size, Slowpath slowpath)
    {
      auto& key = freelist::Object::key_root;
      smallsizeclass_t sizeclass = size_to_sizeclass(size);
      auto& fl = small_fast_free_lists[sizeclass];
      if (SNMALLOC_LIKELY(!fl.empty()))
      {
        auto p = fl.take(key, domesticate);
        return finish_alloc<zero_mem, Config>(p, sizeclass);
      }
      return slowpath(sizeclass, &fl);
    }
  };

} // namespace snmalloc
