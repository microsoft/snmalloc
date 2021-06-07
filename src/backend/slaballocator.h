#pragma once

#include "../mem/metaslab.h"
#include "../mem/remoteallocator.h"
#include "../mem/sizeclass.h"
#include "../mem/sizeclasstable.h"
#include "../ds/mpmcstack.h"
#include "backend.h"


#include <iostream>

namespace snmalloc
{
  /**
   * Used to store slabs in the unused sizes.
   */
  struct SlabRecord
  {
    std::atomic<SlabRecord*> next;
    CapPtr<void, CBChunk> slab;
  };

  /**
   * How many slab sizes that can be provided.
   */
  constexpr size_t NUM_SLAB_SIZES = bits::ADDRESS_BITS - MIN_CHUNK_BITS;

  /**
   * Used to ensure the per slab meta data is large enough for both use cases.
   */
  using MetaBlock = std::conditional<
    (sizeof(MetaEntry) > sizeof(SlabRecord)),
    MetaEntry,
    SlabRecord>;

  /**
   * This is the global state required for the slab allocator.
   * It must be provided as a part of the shared state handle
   * to the slab allocator.
   */
  class SlabAllocatorState
  {
    friend class SlabAllocator;
    /**
     * Stack of slabs that have been returned for reuse.
     */
    ModArray<NUM_SLAB_SIZES, MPMCStack<SlabRecord, RequiresInit>> slab_stack;
  };

  class SlabAllocator
  {
  public:
    template<ZeroMem zero_mem, typename SharedStateHandle>
    static std::pair<CapPtr<void, CBChunk>, Metaslab*> alloc(
      SharedStateHandle h,
      sizeclass_t slab_sizeclass,
      size_t slab_size,
      RemoteAllocator* remote)
    {
      SlabAllocatorState& state = h.get_slab_allocator_state();
      // Pop a slab
      auto slab_record = state.slab_stack[slab_sizeclass].pop();
      CapPtr<void, CBChunk> slab;
      Metaslab* meta;
      if (slab_record != nullptr)
      {
        slab = slab_record->slab;
        meta = reinterpret_cast<Metaslab*>(slab_record);
        return {slab, meta};
      }

      // Allocate a fresh slab as there are no available ones.
      // First create meta-data
      meta = reinterpret_cast<Metaslab*>(
        BackendAllocator::alloc_meta_data<MetaBlock>(h));
      MetaEntry entry{meta, remote};
      slab = BackendAllocator::alloc_slab(h, slab_size, entry);
      return {slab, meta};
    }

    template<typename SharedStateHandle>
    static void
    dealloc(SharedStateHandle h, SlabRecord* p, size_t slab_sizeclass)
    {
      UNUSED(h);
      h.get_slab_allocator_state().slab_stack[slab_sizeclass].push(p);
    }
  };

}