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
    static SNMALLOC_FAST_PATH void alloc_new_list(
      CapPtr<void, CBChunk>& bumpptr,
      FreeListIter& fast_free_list,
      size_t rsize,
      size_t slab_size,
      LocalEntropy& entropy)
    {
      auto slab_end = pointer_offset(bumpptr, slab_size + 1 - rsize);

      FreeListBuilder<false> b;
      SNMALLOC_ASSERT(b.empty());

      b.open(bumpptr);

#ifdef CHECK_CLIENT
      // Structure to represent the temporary list elements
      struct PreAllocObject
      {
        CapPtr<PreAllocObject, CBAlloc> next;
      };
      // The following code implements Sattolo's algorithm for generating
      // random cyclic permutations.  This implementation is in the opposite
      // direction, so that the original space does not need initialising.  This
      // is described as outside-in without citation on Wikipedia, appears to be
      // Folklore algorithm.

      // Note the wide bounds on curr relative to each of the ->next fields;
      // curr is not persisted once the list is built.
      CapPtr<PreAllocObject, CBChunk> curr =
        pointer_offset(bumpptr, 0).template as_static<PreAllocObject>();
      curr->next = Aal::capptr_bound<PreAllocObject, CBAlloc>(curr, rsize);

      uint16_t count = 1;
      for (curr =
             pointer_offset(curr, rsize).template as_static<PreAllocObject>();
           curr.as_void() < slab_end;
           curr =
             pointer_offset(curr, rsize).template as_static<PreAllocObject>())
      {
        size_t insert_index = entropy.sample(count);
        curr->next = std::exchange(
          pointer_offset(bumpptr, insert_index * rsize)
            .template as_static<PreAllocObject>()
            ->next,
          Aal::capptr_bound<PreAllocObject, CBAlloc>(curr, rsize));
        count++;
      }

      // Pick entry into space, and then build linked list by traversing cycle
      // to the start.  Use ->next to jump from CBArena to CBAlloc.
      auto start_index = entropy.sample(count);
      auto start_ptr = pointer_offset(bumpptr, start_index * rsize)
                         .template as_static<PreAllocObject>()
                         ->next;
      auto curr_ptr = start_ptr;
      do
      {
        b.add(FreeObject::make(curr_ptr.as_void()), entropy);
        curr_ptr = curr_ptr->next;
      } while (curr_ptr != start_ptr);
#else
      for (auto p = bumpptr; p < slab_end; p = pointer_offset(p, rsize))
      {
        b.add(Aal::capptr_bound<FreeObject, CBAlloc>(p, rsize), entropy);
      }
#endif
      // This code consumes everything up to slab_end.
      bumpptr = slab_end;

      SNMALLOC_ASSERT(!b.empty());
      b.close(fast_free_list, entropy);
    }

  public:
    template<ZeroMem zero_mem, typename SharedStateHandle>
    static void* alloc(
      SharedStateHandle h,
      sizeclass_t sizeclass,
      RemoteAllocator* remote,
      FreeListIter& fl,
      LocalEntropy& entropy)
    {
      size_t rsize = sizeclass_to_size(sizeclass);
      size_t slab_size = sizeclass_to_slab_size(sizeclass);
      size_t slab_sizeclass = sizeclass_to_slab_sizeclass(sizeclass);

#ifdef SNMALLOC_TRACING
      std::cout << "rsize " << rsize << std::endl;
      std::cout << "slab size " << slab_size << std::endl;
#endif

      SlabAllocatorState& state = h.get_slab_allocator_state();
      // Pop a slab
      auto slab_record = state.slab_stack[slab_sizeclass].pop();
      CapPtr<void, CBChunk> slab;
      Metaslab* meta;
      if (slab_record != nullptr)
      {
        slab = slab_record->slab;
        meta = reinterpret_cast<Metaslab*>(slab_record);
        MetaEntry entry{meta, remote};
        BackendAllocator::set_meta_data(
          h, address_cast(slab), slab_size, entry);
      }
      else
      {
        // Allocate a fresh slab as there are no available ones.
        meta = reinterpret_cast<Metaslab*>(
          BackendAllocator::alloc_meta_data<MetaBlock>(h));
        MetaEntry entry{meta, remote};
        slab = BackendAllocator::alloc_slab(h, slab_size, entry);
      }

      // Build a free list for the slab
      alloc_new_list(slab, fl, rsize, slab_size, entropy);

      // Set meta slab to empty.
      meta->initialise(sizeclass, slab.as_static<Slab>());

      // take an allocation from the free list
      auto p = fl.take(entropy).unsafe_capptr;

      if (zero_mem == YesZero)
      {
        SharedStateHandle::Pal::template zero<false>(p, rsize);
      }

      return p;
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