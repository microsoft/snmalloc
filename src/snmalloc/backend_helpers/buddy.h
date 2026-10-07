#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  /**
   * Class representing a buddy allocator
   *
   * Underlying node `Rep` representation is passed in.
   *
   * The allocator can handle blocks between inclusive MIN_SIZE_BITS and
   * exclusive MAX_SIZE_BITS.
   */
  template<typename Rep, size_t MIN_SIZE_BITS, size_t MAX_SIZE_BITS>
  class Buddy
  {
    static_assert(MAX_SIZE_BITS > MIN_SIZE_BITS);

    struct Entry
    {
      typename Rep::Contents cache[3];
      RBTree<Rep> tree{};
    };

    stl::Array<Entry, MAX_SIZE_BITS - MIN_SIZE_BITS> entries{};
    // All RBtrees at or above this index should be empty.
    size_t empty_at_or_above{0};

    // Tracks the total memory (in bytes) held by this buddy allocator,
    // updated at the API boundary. Debug builds only.
    size_t debug_buddy_total{0};

    size_t to_index(size_t size)
    {
      SNMALLOC_ASSERT(size != 0);
      SNMALLOC_ASSERT(bits::is_pow2(size));
      auto log = snmalloc::bits::next_pow2_bits(size);
      SNMALLOC_ASSERT_MSG(
        log >= MIN_SIZE_BITS, "Size too big: {} log {}.", size, log);
      SNMALLOC_ASSERT_MSG(
        log < MAX_SIZE_BITS, "Size too small: {} log {}.", size, log);

      return log - MIN_SIZE_BITS;
    }

    void validate_block(typename Rep::Contents addr, size_t size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(addr == Rep::align_down(addr, size));
      UNUSED(addr, size);
    }

    /**
     * Walk all levels and sum the memory stored in cache slots and
     * tree nodes.
     */
    size_t debug_count_total()
    {
      size_t total = 0;
      for (size_t i = 0; i < entries.size(); i++)
      {
        size_t block_size = bits::one_at_bit(i + MIN_SIZE_BITS);

        // Count non-null cache entries.
        for (auto& e : entries[i].cache)
        {
          if (!Rep::equal(Rep::null, e))
            total += block_size;
        }

        // Count tree nodes.
        total += entries[i].tree.count() * block_size;
      }
      return total;
    }

    void invariant()
    {
#ifndef NDEBUG
      for (size_t i = empty_at_or_above; i < entries.size(); i++)
      {
        SNMALLOC_ASSERT(entries[i].tree.is_empty());
        for (auto& e : entries[i].cache)
        {
          SNMALLOC_ASSERT(Rep::equal(Rep::null, e));
        }
      }

      auto counted = debug_count_total();
      SNMALLOC_ASSERT_MSG(
        debug_buddy_total == counted,
        "Buddy memory mismatch: tracked={} counted={}",
        debug_buddy_total,
        counted);
#endif
    }

    bool remove_buddy(typename Rep::Contents addr, size_t size)
    {
      auto idx = to_index(size);

      // Empty at this range.
      if (idx >= empty_at_or_above)
        return false;

      auto buddy = Rep::buddy(addr, size);

      // Check local cache first
      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(buddy, e))
        {
          if (!Rep::can_consolidate(addr, size))
            return false;

          e = entries[idx].tree.remove_min();
          return true;
        }
      }

      auto path = entries[idx].tree.get_root_path();
      bool contains_buddy = entries[idx].tree.find(path, buddy);

      if (!contains_buddy)
        return false;

      // Only check if we can consolidate after we know the buddy is in
      // the buddy allocator.  This is required to prevent possible segfaults
      // from looking at the buddies meta-data, which we only know exists
      // once we have found it in the red-black tree.
      if (!Rep::can_consolidate(addr, size))
        return false;

      entries[idx].tree.remove_path(path);
      return true;
    }

  public:
    constexpr Buddy() = default;

    /**
     * Add a block to the buddy allocator.
     *
     * Blocks needs to be power of two size and aligned to the same power of
     * two.
     *
     * Returns null, if the block is successfully added. Otherwise, returns the
     * consolidated block that is MAX_SIZE_BITS big, and hence too large for
     * this allocator.
     */
    typename Rep::Contents add_block(typename Rep::Contents addr, size_t size)
    {
      validate_block(addr, size);
      debug_buddy_total += size;

      if (remove_buddy(addr, size))
      {
        // The buddy (also of `size`) was already tracked when it was
        // originally stored. Remove both halves: the new block we just
        // tracked and the buddy that was already tracked.
        debug_buddy_total -= 2 * size;

        // Add to next level cache
        size *= 2;
        addr = Rep::align_down(addr, size);
        if (size == bits::one_at_bit(MAX_SIZE_BITS))
        {
          // Consolidated block is too large for this allocator —
          // it leaves entirely. Both halves were already subtracted above.
          invariant();
          return addr;
        }
        // Recursively add the consolidated block. The recursive call
        // will do debug_buddy_total += size for the merged block.
        return add_block(addr, size);
      }

      auto idx = to_index(size);
      empty_at_or_above = bits::max(empty_at_or_above, idx + 1);

      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(Rep::null, e))
        {
          e = addr;
          return Rep::null;
        }
      }

      auto path = entries[idx].tree.get_root_path();
      entries[idx].tree.find(path, addr);
      entries[idx].tree.insert_path(path, addr);
      invariant();
      return Rep::null;
    }

    /**
     * Removes a block of size from the buddy allocator.
     *
     * Return Rep::null if this cannot be satisfied.
     */
    typename Rep::Contents remove_block(size_t size)
    {
      invariant();
      auto idx = to_index(size);
      if (idx >= empty_at_or_above)
        return Rep::null;

      auto addr = entries[idx].tree.remove_min();
      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(Rep::null, addr) || Rep::compare(e, addr))
        {
          addr = stl::exchange(e, addr);
        }
      }

      if (addr != Rep::null)
      {
        debug_buddy_total -= size;
        validate_block(addr, size);
        return addr;
      }

      if (size * 2 == bits::one_at_bit(MAX_SIZE_BITS))
        // Too big for this buddy allocator
        return Rep::null;

      auto bigger = remove_block(size * 2);
      if (bigger == Rep::null)
      {
        empty_at_or_above = idx;
        invariant();
        return Rep::null;
      }

      auto second = Rep::offset(bigger, size);

      // Split large block
      add_block(second, size);
      return bigger;
    }
  };
} // namespace snmalloc
