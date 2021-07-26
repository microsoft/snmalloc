#pragma once

#include "../ds/ptrwrap.h"
#include "allocstats.h"
#include "freelist.h"
#include "remotecache.h"
#include "sizeclasstable.h"

#include <string.h>

namespace snmalloc
{
  using Stats = AllocStats<NUM_SIZECLASSES, NUM_LARGE_CLASSES>;

  inline static SNMALLOC_FAST_PATH void* finish_alloc_no_zero(
    snmalloc::CapPtr<snmalloc::FreeObject, snmalloc::CBAlloc> p,
    sizeclass_t sizeclass)
  {
    SNMALLOC_ASSERT(Metaslab::is_start_of_object(sizeclass, address_cast(p)));
    UNUSED(sizeclass);

    auto r = capptr_reveal(capptr_export(p.as_void()));

    return r;
  }

  template<ZeroMem zero_mem, typename SharedStateHandle>
  inline static SNMALLOC_FAST_PATH void* finish_alloc(
    snmalloc::CapPtr<snmalloc::FreeObject, snmalloc::CBAlloc> p,
    sizeclass_t sizeclass)
  {
    auto r = finish_alloc_no_zero(p, sizeclass);

    if constexpr (zero_mem == YesZero)
      SharedStateHandle::Backend::Pal::zero(r, sizeclass_to_size(sizeclass));

    // TODO: Should this be zeroing the FreeObject state, in the non-zeroing
    // case?

    return r;
  }

  // This is defined on its own, so that it can be embedded in the
  // thread local fast allocator, but also referenced from the
  // thread local core allocator.
  struct LocalCache
  {
    // Free list per small size class.  These are used for
    // allocation on the fast path. This part of the code is inspired by
    // mimalloc.
    FreeListIter small_fast_free_lists[NUM_SIZECLASSES];

    // This is the entropy for a particular thread.
    LocalEntropy entropy;

    // TODO: Minimal stats object for just the stats on this datastructure.
    // This will be a zero-size structure if stats are not enabled.
    Stats stats;

    // Pointer to the remote allocator message_queue, used to check
    // if a deallocation is local.
    RemoteAllocator* remote_allocator;

    /**
     * Remote deallocations for other threads
     */
    RemoteDeallocCache remote_dealloc_cache;

    constexpr LocalCache(RemoteAllocator* remote_allocator)
    : remote_allocator(remote_allocator)
    {}

    template<
      size_t allocator_size,
      typename DeallocFun,
      typename SharedStateHandle>
    bool flush(DeallocFun dealloc, SharedStateHandle handle)
    {
      auto& key = entropy.get_free_list_key();

      // Return all the free lists to the allocator.
      // Used during thread teardown
      for (size_t i = 0; i < NUM_SIZECLASSES; i++)
      {
        // TODO could optimise this, to return the whole list in one append
        // call.
        while (!small_fast_free_lists[i].empty())
        {
          auto p = small_fast_free_lists[i].take(key);
          dealloc(finish_alloc_no_zero(p, i));
        }
      }

      return remote_dealloc_cache.post<allocator_size>(
        handle, remote_allocator->trunc_id(), key_global);
    }

    template<ZeroMem zero_mem, typename SharedStateHandle, typename Slowpath>
    SNMALLOC_FAST_PATH void* alloc(size_t size, Slowpath slowpath)
    {
      auto& key = entropy.get_free_list_key();

      sizeclass_t sizeclass = size_to_sizeclass(size);
      stats.alloc_request(size);
      stats.sizeclass_alloc(sizeclass);
      auto& fl = small_fast_free_lists[sizeclass];
      if (likely(!fl.empty()))
      {
        auto p = fl.take(key);
        return finish_alloc<zero_mem, SharedStateHandle>(p, sizeclass);
      }
      return slowpath(sizeclass, &fl);
    }
  };

} // namespace snmalloc