#pragma once

#include "../ds/mpmcstack.h"
#include "../mem/metaslab.h"
#include "../mem/sizeclass.h"
#include "../mem/sizeclasstable.h"
#include "backend.h"

#ifdef SNMALLOC_TRACING
#  include <iostream>
#endif

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
    (sizeof(Metaslab) > sizeof(SlabRecord)),
    Metaslab,
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

    std::atomic<size_t> memory_in_stacks{0};

  public:
    size_t unused_memory()
    {
      return memory_in_stacks;
    }
  };

  class SlabAllocator
  {
  public:
    template<typename SharedStateHandle>
    static std::pair<CapPtr<void, CBChunk>, Metaslab*> alloc(
      SharedStateHandle h,
      AddressSpaceManagerCore<typename SharedStateHandle::Pal>&
        local_address_space,
      sizeclass_t sizeclass,
      sizeclass_t slab_sizeclass, // TODO sizeclass_t
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
        state.memory_in_stacks -= slab_size;
        meta = reinterpret_cast<Metaslab*>(slab_record);
#ifdef SNMALLOC_TRACING
        std::cout << "Reuse slab:" << slab.unsafe_capptr << " slab_sizeclass "
                  << slab_sizeclass << " size " << slab_size
                  << " memory in stacks " << state.memory_in_stacks
                  << std::endl;
#endif
        MetaEntry entry{meta, remote, sizeclass};
        BackendAllocator::set_meta_data(
          h, address_cast(slab), slab_size, entry);
        return {slab, meta};
      }

      // Allocate a fresh slab as there are no available ones.
      // First create meta-data
      meta = reinterpret_cast<Metaslab*>(
        BackendAllocator::alloc_meta_data<MetaBlock>(h, &local_address_space));
      MetaEntry entry{meta, remote, sizeclass};
      slab =
        BackendAllocator::alloc_slab(h, &local_address_space, slab_size, entry);
#ifdef SNMALLOC_TRACING
      std::cout << "Create slab:" << slab.unsafe_capptr << " slab_sizeclass "
                << slab_sizeclass << " size " << slab_size << std::endl;
#endif
      return {slab, meta};
    }

    template<typename SharedStateHandle>
    SNMALLOC_SLOW_PATH static void
    dealloc(SharedStateHandle h, SlabRecord* p, size_t slab_sizeclass)
    {
      auto& state = h.get_slab_allocator_state();
#ifdef SNMALLOC_TRACING
      std::cout << "Return slab:" << p->slab.unsafe_capptr << " slab_sizeclass "
                << slab_sizeclass << " size "
                << slab_sizeclass_to_size(slab_sizeclass)
                << " memory in stacks " << state.memory_in_stacks << std::endl;
#endif
      state.slab_stack[slab_sizeclass].push(p);
      state.memory_in_stacks += slab_sizeclass_to_size(slab_sizeclass);
    }
  };
}