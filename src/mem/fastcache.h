#pragma once

#include "../ds/ptrwrap.h"
#include "allocstats.h"
#include "freelist.h"
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
      SharedStateHandle::Pal::zero(r, sizeclass_to_size(sizeclass));

    return r;
  }

  // This is defined on its own, so that it can be embedded in the
  // thread local fast allocator, but also referenced from the
  // thread local core allocator.
  struct FastCache
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

    /**
     * The total amount of memory we are waiting for before we will dispatch
     * to other allocators. Zero means we have not initialised the allocator
     * yet. This is initialised to the 0 so that we always hit a slow path to
     * start with, when we hit the slow path and need to dispatch everything, we
     * can check if we are a real allocator and lazily provide a real allocator.
     */
    int64_t capacity{0};

    template<typename DeallocFun>
    void flush(DeallocFun dealloc)
    {
      // Return all the free lists to the allocator.
      // Used during thread teardown
      for (size_t i = 0; i < NUM_SIZECLASSES; i++)
      {
        // TODO could optimise this, to return the whole list in one append
        // call.
        while (!small_fast_free_lists[i].empty())
        {
          auto p = small_fast_free_lists[i].take(entropy);
          dealloc(finish_alloc_no_zero(p, i));
        }
      }
    }

    template<ZeroMem zero_mem, typename SharedStateHandle, typename Slowpath>
    SNMALLOC_FAST_PATH void* alloc(size_t size, Slowpath slowpath)
    {
      sizeclass_t sizeclass = size_to_sizeclass(size);
      stats.alloc_request(size);
      stats.sizeclass_alloc(sizeclass);
      auto& fl = small_fast_free_lists[sizeclass];
      if (likely(!fl.empty()))
      {
        auto p = fl.take(entropy);
        return finish_alloc<zero_mem, SharedStateHandle>(p, sizeclass);
      }
      return slowpath(sizeclass, &fl);
    }
  };

} // namespace snmalloc