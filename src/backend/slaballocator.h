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
  static_assert(
    sizeof(Metaslab) >= sizeof(SlabRecord), "We conflat these two types.");

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

    /**
     * All memory issued by this address space manager
     */
    std::atomic<size_t> peak_memory_usage_{0};

    std::atomic<size_t> memory_in_stacks{0};

  public:
    size_t unused_memory()
    {
      return memory_in_stacks;
    }

    size_t peak_memory_usage()
    {
      return peak_memory_usage_;
    }

    void add_peak_memory_usage(size_t size)
    {
      peak_memory_usage_ += size;
#ifdef SNMALLOC_TRACING
      std::cout << "peak_memory_usage_: " << peak_memory_usage_ << std::endl;
#endif
    }
  };

  class SlabAllocator
  {
  public:
    template<typename SharedStateHandle>
    static std::pair<CapPtr<void, CBChunk>, Metaslab*> alloc_slab(
      SharedStateHandle h,
      typename SharedStateHandle::Backend::LocalState& backend_state,
      sizeclass_t sizeclass,
      sizeclass_t slab_sizeclass, // TODO sizeclass_t
      size_t slab_size,
      RemoteAllocator* remote)
    {
      SlabAllocatorState& state = h.get_slab_allocator_state();
      // Pop a slab
      auto slab_record = state.slab_stack[slab_sizeclass].pop();

      if (slab_record != nullptr)
      {
        auto slab = slab_record->slab;
        state.memory_in_stacks -= slab_size;
        auto meta = reinterpret_cast<Metaslab*>(slab_record);
#ifdef SNMALLOC_TRACING
        std::cout << "Reuse slab:" << slab.unsafe_capptr << " slab_sizeclass "
                  << slab_sizeclass << " size " << slab_size
                  << " memory in stacks " << state.memory_in_stacks
                  << std::endl;
#endif
        MetaEntry entry{meta, remote, sizeclass};
        SharedStateHandle::Backend::set_meta_data(
          h.get_backend_state(), address_cast(slab), slab_size, entry);
        return {slab, meta};
      }

      // Allocate a fresh slab as there are no available ones.
      // First create meta-data
      auto [slab, meta] = SharedStateHandle::Backend::alloc_slab(
        h.get_backend_state(), &backend_state, slab_size, remote, sizeclass);
#ifdef SNMALLOC_TRACING
      std::cout << "Create slab:" << slab.unsafe_capptr << " slab_sizeclass "
                << slab_sizeclass << " size " << slab_size << std::endl;
#endif

      state.add_peak_memory_usage(slab_size);
      state.add_peak_memory_usage(sizeof(Metaslab));
      // TODO handle bounded versus lazy pagemaps in stats
      state.add_peak_memory_usage(
        (slab_size / MIN_CHUNK_SIZE) *
        sizeof(typename SharedStateHandle::Meta));

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

    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     */
    template<typename U, typename SharedStateHandle, typename... Args>
    static U* alloc_meta_data(
      SharedStateHandle h,
      typename SharedStateHandle::Backend::LocalState* local_state,
      Args&&... args)
    {
      // Cache line align
      size_t size = bits::align_up(sizeof(U), 64);

      CapPtr<void, CBChunk> p = SharedStateHandle::Backend::alloc_meta_data(
        h.get_backend_state(), local_state, size);

      if (p == nullptr)
        return nullptr;

      return new (p.unsafe_capptr) U(std::forward<Args>(args)...);
    }
  };
}