#pragma once

#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;

  struct SlabLink
  {
    SlabLink* prev;
    SlabLink* next;

    Slab* get_slab()
    {
      return pointer_cast<Slab>(address_cast(this) & SLAB_MASK);
    }
  };

  using SlabList = DLList<SlabLink>;

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab
  {
  public:
    // How many entries are not in the free list of  slab.
    uint16_t used = 0;

    // How many entries have been allocated from this slab.
    uint16_t allocated;

    // Index of first entry in this slab that forms the free
    // list.  The list entries are stored as the first pointer
    // in each unused object. The terminator is a pointer or
    // offset into the block with the bottom bit set.  This means
    //  I.e.
    //    * an empty list has a head of 1.
    //    * a one element list has an head contains an offset to this
    //       this block, and then contains a pointer with the bottom
    //       bit set.
    Mod<SLAB_SIZE, uint16_t> head;

    // When a slab has free space it will be on the has space list for
    // that size class.  We use an empty block in this slab to be the
    // doubly linked node into that size class's free list.
    Mod<SLAB_SIZE, uint16_t> link;

    uint8_t sizeclass;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;

    void add_use()
    {
      used++;
    }

    /**
     * Removes a use, if the slab is either
     *  - empty after removing the use, or
     *  - was full before the substraction
     * this returns true, otherwise returns false.
     **/
    bool sub_use()
    {
      return (--used) == 0;
    }

    bool is_unused()
    {
      return used == 0;
    }

    bool is_full()
    {
      auto result = link == 1;
      assert(!result || head == 1);
      return result;
    }

    void set_full()
    {
      assert(head == 1);
      assert(link != 1);
      link = 1;
      // Set used to 1, so that "sub_use" will return true after calling
      // set_full
      used = 1;
    }

    SlabLink* get_link(Slab* slab)
    {
      return reinterpret_cast<SlabLink*>(pointer_offset(slab, link));
    }

    /// Value used to check for corruptions in a block
    static constexpr size_t POISON =
      static_cast<size_t>(bits::is64() ? 0xDEADBEEFDEADBEEF : 0xDEADBEEF);

    /// Store next pointer in a block. In Debug using magic value to detect some
    /// simple corruptions.
    static SNMALLOC_FAST_PATH void store_next(void* p, void* head)
    {
#ifndef CHECK_CLIENT
      *static_cast<void**>(p) = head;
#else
      *static_cast<void**>(p) = head;
      *(static_cast<uintptr_t*>(p) + 1) = address_cast(head) ^ POISON;
#endif
    }

    /// Accessor function for the next pointer in a block.
    /// In Debug checks for simple corruptions.
    static SNMALLOC_FAST_PATH void* follow_next(void* node)
    {
#ifdef CHECK_CLIENT
      uintptr_t next = *static_cast<uintptr_t*>(node);
      uintptr_t chk = *(static_cast<uintptr_t*>(node) + 1);
      if ((next ^ chk) != POISON)
        error("Detected memory corruption.  Use-after-free.");
#endif
      return *static_cast<void**>(node);
    }

    bool valid_head(bool is_short)
    {
      size_t size = sizeclass_to_size(sizeclass);
      size_t slab_start = get_initial_offset(sizeclass, is_short);
      size_t all_high_bits = ~static_cast<size_t>(1);

      size_t head_start =
        remove_cache_friendly_offset(head & all_high_bits, sizeclass);

      return ((head_start - slab_start) % size) == 0;
    }

    /**
     * Check bump-free-list-segment for cycles
     *
     * Using
     * https://en.wikipedia.org/wiki/Cycle_detection#Floyd's_Tortoise_and_Hare
     * We don't expect a cycle, so worst case is only followed by a crash, so
     * slow doesn't mater.
     **/
    size_t debug_slab_acyclic_free_list(Slab* slab)
    {
#ifndef NDEBUG
      size_t length = 0;
      void* curr = (head == 1) ? nullptr : pointer_offset(slab, head);
      void* curr_slow = (head == 1) ? nullptr : pointer_offset(slab, head);
      bool both = false;
      while (curr != nullptr)
      {
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

    void debug_slab_invariant(bool is_short, Slab* slab)
    {
#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      if (is_full())
      {
        // There is no free list to validate
        // 'link' value is not important if full.
        return;
      }

      if (is_unused())
        return;

      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_initial_offset(sizeclass, is_short);
      size_t accounted_for = used * size + offset;


      // Block is not full
      assert(SLAB_SIZE > accounted_for);

      // Keep variable so it appears in debugger.
      size_t length = debug_slab_acyclic_free_list(slab);
      UNUSED(length);

      // Walk bump-free-list-segment accounting for unused space
      void* curr = (head == 1) ? nullptr : pointer_offset(slab, head);
      while (curr != nullptr)
      {
        // Check we are looking at a correctly aligned block
        void* start = remove_cache_friendly_offset(curr, sizeclass);
        assert(
          ((address_cast(start) - address_cast(slab) - offset) % size) == 0);

        // Account for free elements in free list
        accounted_for += size;
        assert(SLAB_SIZE >= accounted_for);
        // We should never reach the link node in the free list.
        assert(curr != pointer_offset(slab, link));

        // Iterate bump/free list segment
        curr = follow_next(curr);
      }

      auto bumpptr = (allocated * size) + offset;
      // Check we haven't allocaated more than gits in a slab
      assert(bumpptr <= SLAB_SIZE);

      // Account for to be bump allocated space
      accounted_for += SLAB_SIZE - bumpptr;

      if (bumpptr != SLAB_SIZE)
      {
        // The link should be the first allocation as we
        // haven't completely filled this block at any point.
        assert(link == get_initial_offset(sizeclass, is_short));
      }

      assert(!is_full());
      // Add the link node.
      accounted_for += size;

      // All space accounted for
      assert(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
      UNUSED(is_short);
#endif
    }
  };
} // namespace snmalloc
