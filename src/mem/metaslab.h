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
      return (Slab*)((size_t)this & SLAB_MASK);
    }
  };

  using SlabList = DLList<SlabLink, ~(uintptr_t)0>;

  static_assert(
    sizeof(SlabLink) <= MIN_ALLOC_SIZE,
    "Need to be able to pack a SlabLink into any free small alloc");

  static constexpr uint16_t SLABLINK_INDEX =
    (uint16_t)(SLAB_SIZE - sizeof(SlabLink));

  // The Metaslab represent the status of a single slab.
  // This can be either a short or a standard slab.
  class Metaslab
  {
  private:
    // How many entries are used in this slab.
    uint16_t used;

  public:
    // Bump free list of unused entries in this sizeclass.
    // If the bottom bit is 1, then this represents a bump_ptr
    // of where we have allocated up to in this slab. Otherwise,
    // it represents the location of the first block in the free
    // list.  The free list is chained through deallocated blocks.
    // It either terminates with a bump ptr, or if all the space is in
    // the free list, then the last block will be also referenced by
    // link.
    // Note that, in the case that this is the first block in the size
    // class list, where all the unused memory is in the free list,
    // then the last block can both be interpreted as a final bump
    // pointer entry, and the first entry in the doubly linked list.
    // The terminal value in the free list, and the terminal value in
    // the SlabLink previous field will alias. The SlabLink uses ~0 for
    // its terminal value to be a valid terminal bump ptr.
    Mod<SLAB_SIZE, uint16_t> head;
    // When a slab has free space it will be on the has space list for
    // that size class.  We use an empty block in this slab to be the
    // doubly linked node into that size class's free list.
    Mod<SLAB_SIZE, uint16_t> link;

    uint8_t sizeclass;
    uint8_t next;

    void add_use()
    {
      used++;
    }

    void sub_use()
    {
      used--;
    }

    void set_unused()
    {
      used = 0;
    }

    bool is_unused()
    {
      return used == 0;
    }

    bool is_full()
    {
      return (head & 2) != 0;
    }

    void set_full()
    {
      assert(head == 1);
      head = (uint16_t)~0;
    }

    void zero()
    {
      head = 0;
      used = 0;
      link = 0;
      sizeclass = 0;
      next = 0;
    }

    SlabLink* get_link(Slab* slab)
    {
      return (SlabLink*)((size_t)slab + link);
    }

    bool valid_head(bool is_short)
    {
      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_slab_offset(sizeclass, is_short);
      return ((((head & ~(size_t)1) - (offset & ~(size_t)1)) % size) == 0);
    }

    void debug_slab_invariant(bool is_short, Slab* slab)
    {
#if !defined(NDEBUG) && !defined(SNMALLOC_CHEAP_CHECKS)
      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_slab_offset(sizeclass, is_short) - 1;

      size_t accounted_for = used * size + offset;

      if (is_full())
      {
        // All the blocks must be used.
        assert(SLAB_SIZE == accounted_for);
        // There is no free list to validate
        // 'link' value is not important if full.
        return;
      }
      // Block is not full
      assert(SLAB_SIZE > accounted_for);

      // Walk bump-free-list-segment accounting for unused space
      uint16_t curr = head;
      while ((curr & 1) != 1)
      {
        // Check we are looking at a correctly aligned block
        assert((curr - offset) % size == 0);
        // Account for free elements in free list
        accounted_for += size;
        assert(SLAB_SIZE >= accounted_for);
        // We are not guaranteed to hit a bump ptr unless
        // we are the top element on the size class, so treat as
        // a list segment.
        if (curr == link)
          break;
        // Iterate bump/free list segment
        curr = *(uint16_t*)((uintptr_t)slab + curr);
      }

      // Check we terminated traversal on a correctly aligned block
      assert(((curr & ~1) - offset) % size == 0);

      if (curr != link)
      {
        // The link should be at the special end location as we
        // haven't completely filled this block at any point.
        assert(link == SLABLINK_INDEX);
        // Account for to be bump allocated space
        accounted_for += SLAB_SIZE - (curr - 1);
      }

      // All space accounted for
      assert(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
      UNUSED(is_short);
#endif
    }
  };
}
