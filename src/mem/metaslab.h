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
      return pointer_align_down<SLAB_SIZE, Slab>(this);
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
    /**
     *  Pointer to first free entry in this slab
     *
     *  The list will be (allocated - needed - 1) long. The -1 is
     *  for the `link` element which is not in the free list.
     */
    void* head;

    /**
     *  How many entries are not in the free list of slab, i.e.
     *  how many entries are needed to fully free this slab.
     *
     *  In the case of a fully allocated slab, where link==1 needed
     *  will be 1. This enables 'return_object' to detect the slow path
     *  case with a single operation subtract and test.
     */
    uint16_t needed = 0;

    /**
     *  How many entries have been allocated from this slab.
     */
    uint16_t allocated;

    // When a slab has free space it will be on the has space list for
    // that size class.  We use an empty block in this slab to be the
    // doubly linked node into that size class's free list.
    Mod<SLAB_SIZE, uint16_t> link;

    uint8_t sizeclass;
    // Initially zero to encode the superslabs relative list of slabs.
    uint8_t next = 0;

    /**
     * Updates statistics for adding an entry to the free list, if the
     * slab is either
     *  - empty adding the entry to the free list, or
     *  - was full before the subtraction
     * this returns true, otherwise returns false.
     **/
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
      auto result = link == 1;
      assert(!result || head == nullptr);
      return result;
    }

    void set_full()
    {
      assert(head == nullptr);
      assert(link != 1);
      link = 1;
      // Set needed to 1, so that "return_object" will return true after calling
      // set_full
      needed = 1;
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

    bool valid_head()
    {
      size_t size = sizeclass_to_size(sizeclass);
      size_t slab_end = (address_cast(head) | ~SLAB_MASK) + 1;
      uintptr_t allocation_start =
        remove_cache_friendly_offset(address_cast(head), sizeclass);

      return (slab_end - allocation_start) % size == 0;
    }

    static Slab* get_slab(void* p)
    {
      return pointer_cast<Slab>(address_cast(p) & SLAB_MASK);
    }

    static bool is_short(Slab* p)
    {
      return (address_cast(p) & SUPERSLAB_MASK) == address_cast(p);
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
      void* curr = head;
      void* curr_slow = head;
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
        // 'link' value is not important if full.
        return;
      }

      if (is_unused())
        return;

      size_t size = sizeclass_to_size(sizeclass);
      size_t offset = get_initial_offset(sizeclass, is_short);
      size_t accounted_for = needed * size + offset;

      // Block is not full
      assert(SLAB_SIZE > accounted_for);

      // Keep variable so it appears in debugger.
      size_t length = debug_slab_acyclic_free_list(slab);
      UNUSED(length);

      // Walk bump-free-list-segment accounting for unused space
      void* curr = head;
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
#endif
    }
  };
} // namespace snmalloc
