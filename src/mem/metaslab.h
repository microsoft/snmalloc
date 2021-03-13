#pragma once

#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "freelist.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;

  using SlabList = CDLLNode<>;
  using SlabLink = CDLLNode<>;

  SNMALLOC_FAST_PATH Slab* get_slab(SlabLink* sl)
  {
    return pointer_align_down<SLAB_SIZE, Slab>(sl);
  }

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab : public SlabLink
  {
  public:
    /**
     *  Pointer to first free entry in this slab
     *
     *  The list will be (allocated - needed) long.
     */
    FreeListBuilder free_queue;

    /**
     *  How many entries are not in the free list of slab, i.e.
     *  how many entries are needed to fully free this slab.
     *
     *  In the case of a fully allocated slab, where prev==0 needed
     *  will be 1. This enables 'return_object' to detect the slow path
     *  case with a single operation subtract and test.
     */
    uint16_t needed = 0;

    /**
     *  How many entries have been allocated from this slab.
     */
    uint16_t allocated = 0;

    uint8_t sizeclass;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;

    /**
     * Updates statistics for adding an entry to the free list, if the
     * slab is either
     *  - empty adding the entry to the free list, or
     *  - was full before the subtraction
     * this returns true, otherwise returns false.
     */
    bool return_object()
    {
      return (--needed) == 0;
    }

    bool is_unused()
    {
      return needed == 0;
    }

    bool is_full()
    {
      auto result = get_prev() == nullptr;
      SNMALLOC_ASSERT(!result || free_queue.empty());
      return result;
    }

    SNMALLOC_FAST_PATH void set_full()
    {
      SNMALLOC_ASSERT(free_queue.empty());
      // Set needed to 1, so that "return_object" will return true after calling
      // set_full
      needed = 1;
      null_prev();
    }

    bool valid_head()
    {
      size_t size = sizeclass_to_size(sizeclass);
      auto h = address_cast(free_queue.peek_head());
      address_t slab_end = (h | ~SLAB_MASK) + 1;
      address_t allocation_start = remove_cache_friendly_offset(h, sizeclass);

      return (slab_end - allocation_start) % size == 0;
    }

    static Slab* get_slab(const void* p)
    {
      return pointer_align_down<SLAB_SIZE, Slab>(const_cast<void*>(p));
    }

    static bool is_short(Slab* p)
    {
      return pointer_align_down<SUPERSLAB_SIZE>(p) == p;
    }

    SNMALLOC_FAST_PATH static bool is_start_of_object(Metaslab* self, void* p)
    {
      return is_multiple_of_sizeclass(
        sizeclass_to_size(self->sizeclass),
        pointer_diff(p, pointer_align_up<SLAB_SIZE>(pointer_offset(p, 1))));
    }

    /**
     * Takes a free list out of a slabs meta data.
     * Returns the link as the allocation, and places the free list into the
     * `fast_free_list` for further allocations.
     *
     * This is pre-factored to take an explicit self parameter so that we can
     * eventually annotate that pointer with additional information.
     */
    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    static SNMALLOC_FAST_PATH void*
    alloc(Metaslab* self, FreeListIter& fast_free_list, size_t rsize)
    {
      SNMALLOC_ASSERT(rsize == sizeclass_to_size(self->sizeclass));
      SNMALLOC_ASSERT(!self->is_full());

      auto slab = get_slab(self->free_queue.peek_head());

      self->debug_slab_invariant(slab);

      self->free_queue.close(fast_free_list);
      void* n = fast_free_list.take();

      // Treat stealing the free list as allocating it all.
      self->needed = self->allocated;
      self->remove();
      self->set_full();

      void* p = remove_cache_friendly_offset(n, self->sizeclass);
      SNMALLOC_ASSERT(is_start_of_object(self, p));

      self->debug_slab_invariant(slab);

      if constexpr (zero_mem == YesZero)
      {
        if (rsize < PAGE_ALIGNED_SIZE)
          PAL::zero(p, rsize);
        else
          PAL::template zero<true>(p, rsize);
      }
      else
      {
        UNUSED(rsize);
      }

      return p;
    }

    void debug_slab_invariant(Slab* slab)
    {
#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      bool is_short = Metaslab::is_short(slab);

      if (is_full())
      {
        // There is no free list to validate
        return;
      }

      if (is_unused())
        return;

      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_initial_offset(sizeclass, is_short);
      size_t accounted_for = needed * size + offset;

      // Block is not full
      SNMALLOC_ASSERT(SLAB_SIZE > accounted_for);

      // Walk bump-free-list-segment accounting for unused space
      FreeListIter fl = free_queue.terminate();

      while (!fl.empty())
      {
        // Check we are looking at a correctly aligned block
        void* start = remove_cache_friendly_offset(fl.take(), sizeclass);
        SNMALLOC_ASSERT(((pointer_diff(slab, start) - offset) % size) == 0);

        // Account for free elements in free list
        accounted_for += size;
        SNMALLOC_ASSERT(SLAB_SIZE >= accounted_for);
      }

      auto bumpptr = (allocated * size) + offset;
      // Check we haven't allocaated more than gits in a slab
      SNMALLOC_ASSERT(bumpptr <= SLAB_SIZE);

      // Account for to be bump allocated space
      accounted_for += SLAB_SIZE - bumpptr;

      SNMALLOC_ASSERT(!is_full());

      // All space accounted for
      SNMALLOC_ASSERT(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
#endif
    }
  };
} // namespace snmalloc
