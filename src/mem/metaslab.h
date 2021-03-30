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

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  /**
   * This struct is used inside FreeListBuilder to account for the
   * alignment space that is wasted in sizeof.
   *
   * This is part of Metaslab abstraction.
   */
  struct MetaslabEnd
  {
    /**
     *  How many entries are not in the free list of slab, i.e.
     *  how many entries are needed to fully free this slab.
     *
     *  In the case of a fully allocated slab, where prev==0 needed
     *  will be 1. This enables 'return_object' to detect the slow path
     *  case with a single operation subtract and test.
     */
    uint16_t needed = 0;

    uint8_t sizeclass;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;
  };

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab : public SlabLink
  {
  public:
    /**
     *  Data-structure for building the free list for this slab.
     *
     *  Spare 32bits are used for the fields in MetaslabEnd.
     */
    FreeListBuilder<MetaslabEnd> free_queue;

    uint16_t& needed()
    {
      return free_queue.s.needed;
    }

    uint8_t sizeclass()
    {
      return free_queue.s.sizeclass;
    }

    uint8_t& next()
    {
      return free_queue.s.next;
    }

    void initialise(sizeclass_t sizeclass, Slab* slab)
    {
      free_queue.s.sizeclass = static_cast<uint8_t>(sizeclass);
      free_queue.init();
      // Set up meta data as if the entire slab has been turned into a free
      // list. This means we don't have to check for special cases where we have
      // returned all the elements, but this is a slab that is still being bump
      // allocated from. Hence, the bump allocator slab will never be returned
      // for use in another size class.
      set_full(slab);
    }

    /**
     * Updates statistics for adding an entry to the free list, if the
     * slab is either
     *  - empty adding the entry to the free list, or
     *  - was full before the subtraction
     * this returns true, otherwise returns false.
     */
    bool return_object()
    {
      return (--needed()) == 0;
    }

    bool is_unused()
    {
      return needed() == 0;
    }

    bool is_full()
    {
      return get_prev() == nullptr;
    }

    /**
     * Only wake slab if we have this many free allocations
     *
     * This helps remove bouncing around empty to non-empty cases.
     *
     * It also increases entropy, when we have randomisation.
     */
    uint16_t threshold_for_waking_slab(bool is_short_slab)
    {
      auto capacity = get_slab_capacity(sizeclass(), is_short_slab);
      uint16_t threshold = (capacity / 8) | 1;
      uint16_t max = 32;
      return bits::min(threshold, max);
    }

    SNMALLOC_FAST_PATH void set_full(Slab* slab)
    {
      SNMALLOC_ASSERT(free_queue.empty());

      // Prepare for the next free queue to be built.
      free_queue.open(slab);

      // Set needed to at least one, possibly more so we only use
      // a slab when it has a reasonable amount of free elements
      needed() = threshold_for_waking_slab(Metaslab::is_short(slab));
      null_prev();
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
        self->sizeclass(),
        SLAB_SIZE - pointer_diff(pointer_align_down<SLAB_SIZE>(p), p));
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
    static SNMALLOC_FAST_PATH void* alloc(
      Metaslab* self,
      FreeListIter& fast_free_list,
      size_t rsize,
      LocalEntropy& entropy)
    {
      SNMALLOC_ASSERT(rsize == sizeclass_to_size(self->sizeclass()));
      SNMALLOC_ASSERT(!self->is_full());

      self->free_queue.close(fast_free_list, entropy);
      void* n = fast_free_list.take(entropy);

      entropy.refresh_bits();

      // Treat stealing the free list as allocating it all.
      self->remove();
      self->set_full(Metaslab::get_slab(n));

      void* p = remove_cache_friendly_offset(n, self->sizeclass());
      SNMALLOC_ASSERT(is_start_of_object(self, p));

      self->debug_slab_invariant(Metaslab::get_slab(p), entropy);

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

    void debug_slab_invariant(Slab* slab, LocalEntropy& entropy)
    {
#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      bool is_short = Metaslab::is_short(slab);

      if (is_full())
      {
        size_t count = free_queue.debug_length(entropy);
        SNMALLOC_ASSERT(count < threshold_for_waking_slab(is_short));
        return;
      }

      if (is_unused())
        return;

      size_t size = sizeclass_to_size(sizeclass());
      size_t offset = get_initial_offset(sizeclass(), is_short);
      size_t accounted_for = needed() * size + offset;

      // Block is not full
      SNMALLOC_ASSERT(SLAB_SIZE > accounted_for);

      // Account for list size
      size_t count = free_queue.debug_length(entropy);
      accounted_for += count * size;

      SNMALLOC_ASSERT(count <= get_slab_capacity(sizeclass(), is_short));

      auto bumpptr = (get_slab_capacity(sizeclass(), is_short) * size) + offset;
      // Check we haven't allocated more than fits in a slab
      SNMALLOC_ASSERT(bumpptr <= SLAB_SIZE);

      // Account for to be bump allocated space
      accounted_for += SLAB_SIZE - bumpptr;

      SNMALLOC_ASSERT(!is_full());

      // All space accounted for
      SNMALLOC_ASSERT(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
      UNUSED(entropy);
#endif
    }
  };
} // namespace snmalloc
