#pragma once

#include "../ds/ds.h"

namespace snmalloc
{
  /**
   * Default no-op histogram hook for `Buddy`.  Whenever a free block is
   * inserted into or removed from the buddy allocator's per-bucket
   * cache/tree, the buddy invokes `Histogram::on_add(size_bits)` /
   * `Histogram::on_remove(size_bits)`.  The default specialisation is
   * empty so callers (e.g. `SmallBuddyRange`) that do not want to track
   * a histogram pay zero overhead -- the inlined no-op compiles away.
   */
  struct BuddyNoHistogram
  {
    static void on_add(size_t /*size_bits*/) {}

    static void on_remove(size_t /*size_bits*/) {}
  };

  /**
   * Class representing a buddy allocator
   *
   * Underlying node `Rep` representation is passed in.
   *
   * The allocator can handle blocks between inclusive MIN_SIZE_BITS and
   * exclusive MAX_SIZE_BITS.
   *
   * `Histogram` is a free-chunk-count callback hook with two static
   * methods (`on_add(size_bits)` / `on_remove(size_bits)`) invoked
   * whenever the per-bucket cache/tree population changes by one.  The
   * default `BuddyNoHistogram` is a pair of no-ops; `LargeBuddyRange`
   * substitutes a process-global atomic histogram so the Phase 11.4
   * FullAllocStats getter can report a log2-bucketed view of free
   * chunks.
   */
  template<
    typename Rep,
    size_t MIN_SIZE_BITS,
    size_t MAX_SIZE_BITS,
    typename Histogram = BuddyNoHistogram>
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

    void invariant()
    {
#ifndef NDEBUG
      for (size_t i = empty_at_or_above; i < entries.size(); i++)
      {
        SNMALLOC_ASSERT(entries[i].tree.is_empty());
        // TODO check cache is empty
      }
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
          // One free block leaves the system at this bucket: either the
          // matched cache slot is overwritten with the tree's minimum
          // (so the tree shrinks by one) or, if the tree was already
          // empty, `remove_min` returns `Rep::null` and the slot
          // becomes null.  Both branches net to -1 entry at `idx`.
          Histogram::on_remove(MIN_SIZE_BITS + idx);
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
      Histogram::on_remove(MIN_SIZE_BITS + idx);
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

      if (remove_buddy(addr, size))
      {
        // Add to next level cache
        size *= 2;
        addr = Rep::align_down(addr, size);
        if (size == bits::one_at_bit(MAX_SIZE_BITS))
        {
          // Invariant should be checked on all non-tail return paths.
          // Holds trivially here with current design.
          invariant();
          // Too big for this buddy allocator.
          return addr;
        }
        return add_block(addr, size);
      }

      auto idx = to_index(size);
      empty_at_or_above = bits::max(empty_at_or_above, idx + 1);

      for (auto& e : entries[idx].cache)
      {
        if (Rep::equal(Rep::null, e))
        {
          e = addr;
          // One new free block enters the system at this bucket via
          // the inline cache.
          Histogram::on_add(MIN_SIZE_BITS + idx);
          return Rep::null;
        }
      }

      auto path = entries[idx].tree.get_root_path();
      entries[idx].tree.find(path, addr);
      entries[idx].tree.insert_path(path, addr);
      // One new free block enters the system at this bucket via the
      // red-black tree (cache slots were all full).
      Histogram::on_add(MIN_SIZE_BITS + idx);
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
        validate_block(addr, size);
        // One free block leaves the system at this bucket -- either
        // popped directly from the tree (when `tree.remove_min` was
        // non-null) or selected from a cache slot via the swap loop
        // above.  Either way, the net population at `idx` falls by 1.
        Histogram::on_remove(MIN_SIZE_BITS + idx);
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
