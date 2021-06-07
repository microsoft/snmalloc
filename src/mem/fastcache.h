#pragma once

#include "../ds/ptrwrap.h"
#include "allocstats.h"
#include "freelist.h"
#include "sizeclasstable.h"

#include <string.h>

namespace snmalloc
{
  using Stats = AllocStats<NUM_SIZECLASSES, NUM_LARGE_CLASSES>;

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
          dealloc(small_fast_free_lists[i].take(entropy).unsafe_capptr);
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
        auto r = capptr_reveal(capptr_export(p.as_void()));
        if constexpr (zero_mem == YesZero)
          SharedStateHandle::Pal::zero(r, sizeclass_to_size(sizeclass));

        return r;
      }
      return slowpath(sizeclass, &fl);
    }
  };

} // namespace snmalloc