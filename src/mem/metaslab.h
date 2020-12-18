#pragma once

#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "sizeclass.h"

namespace snmalloc
{
  class Slab;
  class FreeListEntry;

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
  class Metaslab
  {
  public:
    /**
     *  Pointer to first free entry in this slab.  Recall that free lists
     *  contain already-bounded objects.
     *
     *  The list will be (allocated - needed - 1) long. The -1 is
     *  for the `link` element which is not in the free list.
     */
    FreePtr<FreeListEntry> head = nullptr;

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
    uint16_t allocated = 0;

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
      auto result = link == 1;
      SNMALLOC_ASSERT(!result || head == nullptr);
      return result;
    }

    SNMALLOC_FAST_PATH void set_full()
    {
      SNMALLOC_ASSERT(head == nullptr);
      SNMALLOC_ASSERT(link != 1);
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
    static SNMALLOC_FAST_PATH void
    store_next(FreePtr<FreeListEntry> p, FreePtr<FreeListEntry> head)
    {
      *reinterpret_cast<FreePtr<FreeListEntry>*>(p.unsafe_free_ptr) = head;
#if defined(CHECK_CLIENT)
      if constexpr (aal_supports<IntegerPointers>)
      {
        *(reinterpret_cast<uintptr_t*>(p.unsafe_free_ptr) + 1) =
          address_cast(head.unsafe_free_ptr) ^ POISON;
      }
#endif
    }

    /// Accessor function for the next pointer in a block.
    /// In Debug checks for simple corruptions.
    static SNMALLOC_FAST_PATH FreePtr<FreeListEntry>
    follow_next(FreePtr<FreeListEntry> node)
    {
#if defined(CHECK_CLIENT)
      if constexpr (aal_supports<IntegerPointers>)
      {
        uintptr_t next = *reinterpret_cast<uintptr_t*>(node.unsafe_free_ptr);
        uintptr_t chk =
          *(reinterpret_cast<uintptr_t*>(node.unsafe_free_ptr) + 1);
        if ((next ^ chk) != POISON)
          error("Detected memory corruption.  Use-after-free.");
      }
#endif
      return *reinterpret_cast<FreePtr<FreeListEntry>*>(node.unsafe_free_ptr);
    }

    bool valid_head()
    {
      size_t size = sizeclass_to_size(sizeclass);
      address_t head_addr = address_cast(head);
      size_t slab_end = (head_addr | ~SLAB_MASK) + 1;
      uintptr_t allocation_start =
        remove_cache_friendly_offset(head_addr, sizeclass);

      return (slab_end - allocation_start) % size == 0;
    }

    template<typename T = void>
    static Slab* get_slab(AuthPtr<T> a)
    {
      return pointer_align_down<SLAB_SIZE, Slab>(a.unsafe_auth_ptr);
    }

    /**
     * Compute where the Slab metadata would be, but without any expectation
     * that the resulting pointer be authorized to access said metadata.
     * Useful almost exclusively for debugging.
     */
    static Slab* get_slab_noauth(void* p)
    {
      return get_slab(mk_authptr<void>(p));
    }

    static bool is_short(Slab* p)
    {
      return pointer_align_down<SUPERSLAB_SIZE>(p) == p;
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
      FreePtr<FreeListEntry> curr = head;
      FreePtr<FreeListEntry> curr_slow = head;
      bool both = false;
      while (curr != nullptr)
      {
        if (get_slab_noauth(curr.unsafe_free_ptr) != slab)
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
      SNMALLOC_ASSERT(SLAB_SIZE > accounted_for);

      // Keep variable so it appears in debugger.
      size_t length = debug_slab_acyclic_free_list(slab);
      UNUSED(length);

      // Walk bump-free-list-segment accounting for unused space
      FreePtr<FreeListEntry> curr = head;
      while (curr != nullptr)
      {
        // Check we are looking at a correctly aligned block
        FreePtr<void> start = remove_cache_friendly_offset(curr, sizeclass);
        SNMALLOC_ASSERT(
          ((pointer_diff(slab, start.unsafe_free_ptr) - offset) % size) == 0);

        // Account for free elements in free list
        accounted_for += size;
        SNMALLOC_ASSERT(SLAB_SIZE >= accounted_for);
        // We should never reach the link node in the free list.
        SNMALLOC_ASSERT(
          curr.unsafe_free_ptr !=
          reinterpret_cast<FreeListEntry*>(pointer_offset(slab, link)));

        // Iterate bump/free list segment
        curr = follow_next(curr);
      }

      auto bumpptr = (allocated * size) + offset;
      // Check we haven't allocaated more than gits in a slab
      SNMALLOC_ASSERT(bumpptr <= SLAB_SIZE);

      // Account for to be bump allocated space
      accounted_for += SLAB_SIZE - bumpptr;

      if (bumpptr != SLAB_SIZE)
      {
        // The link should be the first allocation as we
        // haven't completely filled this block at any point.
        SNMALLOC_ASSERT(link == get_initial_offset(sizeclass, is_short));
      }

      SNMALLOC_ASSERT(!is_full());
      // Add the link node.
      accounted_for += size;

      // All space accounted for
      SNMALLOC_ASSERT(SLAB_SIZE == accounted_for);
#else
      UNUSED(slab);
#endif
    }
  };
} // namespace snmalloc
