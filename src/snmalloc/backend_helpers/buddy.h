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
    std::array<RBTree<Rep>, MAX_SIZE_BITS - MIN_SIZE_BITS> trees;

    size_t to_index(size_t size)
    {
      auto log = snmalloc::bits::next_pow2_bits(size);
      SNMALLOC_ASSERT(log >= MIN_SIZE_BITS);
      SNMALLOC_ASSERT(log < MAX_SIZE_BITS);

      return log - MIN_SIZE_BITS;
    }

    void validate_block(typename Rep::Contents addr, size_t size)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(addr == Rep::align_down(addr, size));
      UNUSED(addr, size);
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
      auto idx = to_index(size);

      validate_block(addr, size);

      auto buddy = Rep::buddy(addr, size);

      auto path = trees[idx].get_root_path();
      bool contains_buddy = trees[idx].find(path, buddy);

      if (contains_buddy)
      {
        // Only check if we can consolidate after we know the buddy is in
        // the buddy allocator.  This is required to prevent possible segfaults
        // from looking at the buddies meta-data, which we only know exists
        // once we have found it in the red-black tree.
        if (Rep::can_consolidate(addr, size))
        {
          trees[idx].remove_path(path);

          // Add to next level cache
          size *= 2;
          addr = Rep::align_down(addr, size);
          if (size == bits::one_at_bit(MAX_SIZE_BITS))
            // Too big for this buddy allocator.
            return addr;
          return add_block(addr, size);
        }

        // Re-traverse as the path was to the buddy,
        // but the representation says we cannot combine.
        // We must find the correct place for this element.
        // Something clever could be done here, but it's not worth it.
        //        path = trees[idx].get_root_path();
        trees[idx].find(path, addr);
      }
      trees[idx].insert_path(path, addr);
      return Rep::null;
    }

    /**
     * Removes a block of size from the buddy allocator.
     *
     * Return Rep::null if this cannot be satisfied.
     */
    typename Rep::Contents remove_block(size_t size)
    {
      auto idx = to_index(size);

      auto addr = trees[idx].remove_min();
      if (addr != Rep::null)
      {
        validate_block(addr, size);
        return addr;
      }

      if (size * 2 == bits::one_at_bit(MAX_SIZE_BITS))
        // Too big for this buddy allocator
        return Rep::null;

      auto bigger = remove_block(size * 2);
      if (bigger == Rep::null)
        return Rep::null;

      auto second = Rep::offset(bigger, size);

      // Split large block
      add_block(second, size);
      return bigger;
    }
  };
} // namespace snmalloc
