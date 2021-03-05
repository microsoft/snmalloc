#pragma once

#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;

  /**
   * Free objects within each slab point directly to the next (contrast
   * SlabLink, which chain different Slabs of the same sizeclass together).
   */
  struct SlabNext
  {
    struct SlabNext* next;
#ifdef CHECK_CLIENT
    uintptr_t guard;
#endif
  };

  struct FreeListHead
  {
    // Use a value with bottom bit set for empty list.
    SlabNext* value = nullptr;
  };

  using SlabList = CDLLNode;
  using SlabLink = CDLLNode;

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
    SlabNext* head = nullptr;

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
      SNMALLOC_ASSERT(!result || head == nullptr);
      return result;
    }

    SNMALLOC_FAST_PATH void set_full()
    {
      SNMALLOC_ASSERT(head == nullptr);
      // Set needed to 1, so that "return_object" will return true after calling
      // set_full
      needed = 1;
      null_prev();
    }

    /// Value used to check for corruptions in a block
    static constexpr size_t POISON =
      static_cast<size_t>(bits::is64() ? 0xDEADBEEFDEADBEEF : 0xDEADBEEF);

    /// Store next pointer in a block. In Debug using magic value to detect some
    /// simple corruptions.
    static SNMALLOC_FAST_PATH void store_next(SlabNext* p, SlabNext* head)
    {
      p->next = head;
#if defined(CHECK_CLIENT)
      if constexpr (aal_supports<IntegerPointers>)
      {
        p->guard = address_cast(head) ^ POISON;
      }
#endif
    }

    /// Accessor function for the next pointer in a block.
    /// In Debug checks for simple corruptions.
    static SNMALLOC_FAST_PATH SlabNext* follow_next(SlabNext* node)
    {
#if defined(CHECK_CLIENT)
      if constexpr (aal_supports<IntegerPointers>)
      {
        uintptr_t next = address_cast(node->next);
        if ((next ^ node->guard) != POISON)
          error("Detected memory corruption.  Use-after-free.");
      }
#endif
      return node->next;
    }

    bool valid_head()
    {
      size_t size = sizeclass_to_size(sizeclass);
      size_t slab_end = (address_cast(head) | ~SLAB_MASK) + 1;
      uintptr_t allocation_start =
        remove_cache_friendly_offset(address_cast(head), sizeclass);

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

    bool is_start_of_object(void* p)
    {
      return is_multiple_of_sizeclass(
        sizeclass_to_size(sizeclass),
        pointer_diff(p, pointer_align_up<SLAB_SIZE>(pointer_offset(p, 1))));
    }

    /**
     * Takes a free list out of a slabs meta data.
     * Returns the link as the allocation, and places the free list into the
     * `fast_free_list` for further allocations.
     */
    template<ZeroMem zero_mem, SNMALLOC_CONCEPT(ConceptPAL) PAL>
    SNMALLOC_FAST_PATH void* alloc(FreeListHead& fast_free_list, size_t rsize)
    {
      SNMALLOC_ASSERT(rsize == sizeclass_to_size(sizeclass));
      SNMALLOC_ASSERT(!is_full());

      auto slab = get_slab(head);
      debug_slab_invariant(slab);

      // Use first element as the allocation
      SlabNext* h = head;
      // Put the rest in allocators small_class fast free list.
      fast_free_list.value = Metaslab::follow_next(h);
      head = nullptr;

      // Treat stealing the free list as allocating it all.
      needed = allocated;
      remove();
      set_full();

      void* p = remove_cache_friendly_offset(h, sizeclass);
      SNMALLOC_ASSERT(is_start_of_object(p));

      debug_slab_invariant(slab);

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

    /**
     * Check bump-free-list-segment for cycles
     *
     * Using
     * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
     * We don't expect a cycle, so worst case is only followed by a crash, so
     * slow doesn't mater.
     */
    size_t debug_slab_acyclic_free_list(Slab* slab)
    {
#ifndef NDEBUG
      size_t length = 0;
      SlabNext* curr = head;
      SlabNext* curr_slow = head;
      bool both = false;
      while (curr != nullptr)
      {
        if (get_slab(curr) != slab)
        {
          error("Free list corruption, not correct slab.");
        }
        curr = follow_next(curr);
        if (both)
        {
          curr_slow = follow_next(curr_slow);
        }

        if (curr == curr_slow)
        {
          error("Free list contains a cycle, typically indicates double free.");
        }

        both = !both;
        length++;
      }
      return length;
#else
      UNUSED(slab);
      return 0;
#endif
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

      // Keep variable so it appears in debugger.
      size_t length = debug_slab_acyclic_free_list(slab);
      UNUSED(length);

      // Walk bump-free-list-segment accounting for unused space
      SlabNext* curr = head;
      while (curr != nullptr)
      {
        // Check we are looking at a correctly aligned block
        void* start = remove_cache_friendly_offset(curr, sizeclass);
        SNMALLOC_ASSERT(((pointer_diff(slab, start) - offset) % size) == 0);

        // Account for free elements in free list
        accounted_for += size;
        SNMALLOC_ASSERT(SLAB_SIZE >= accounted_for);

        // Iterate bump/free list segment
        curr = follow_next(curr);
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
