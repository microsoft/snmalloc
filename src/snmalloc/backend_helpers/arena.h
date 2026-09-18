#pragma once

#include "../ds_core/redblacktree.h"
#include "../ds_core/sizeclassconfig.h"
#include "../stl/array.h"
#include "../stl/utility.h"
#include "arenabins.h"

#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
  struct ArenaTestAccess;

  /**
   * Size encoding for a free block's first pagemap entry.
   * Min:     exactly 1 chunk (no range-tree entry).
   * EvenTwo: exactly 2 chunks, 2-aligned; can serve size-2 requests.
   * OddTwo:  exactly 2 chunks, NOT 2-aligned; in range tree but
   *          placed in a size-1 bin (cannot serve aligned size-2 requests).
   * Large:   3+ chunks; precise size stored in a separate entry.
   */
  enum class ArenaVariant : uint8_t
  {
    Min = 0,
    EvenTwo = 1,
    OddTwo = 2,
    Large = 3
  };

  /**
   * Manages free ranges within a single bounded arena using a dual-tree
   * scheme: a set of bin trees indexed by the floor-log2 size class
   * (used for allocation lookup) and one range tree keyed by address
   * (used for consolidation of adjacent free ranges).
   *
   * `Rep` is the representation. It owns *all* storage and bit-layout
   * decisions for tree nodes and per-block metadata. `Rep` must provide:
   *
   *   - `using BinRep`  — full RBTree Rep for the bin trees, supplying
   *     `Handle`, `Contents`, `null`, `root`, `ref`, `get`, `set`,
   *     `is_red`, `set_red`, `compare`, `equal`, `printable`, `name`.
   *     Owns its own red-bit packing privately.
   *   - `using RangeRep` — full RBTree Rep for the range tree, same
   *     shape as `BinRep`.
   *   - `get_variant(addr)` / `set_variant(addr, v)` — the
   *     `ArenaVariant` tag for the block starting at `addr`.
   *   - `get_large_size(addr)` / `set_large_size(addr, size)` —
   *     exact byte size for `Large` blocks (3+ units).
   *   - `can_consolidate(higher_addr) -> bool` — whether the block at
   *     `higher_addr` may be merged with the block immediately below
   *     it. Returns false at allocation boundaries that must be
   *     preserved.
   *
   * `MIN_SIZE_BITS`: log2 of the unit of allocation (= the minimum
   * block size in bytes). All addresses and sizes managed by this
   * arena are multiples of `1 << MIN_SIZE_BITS`.
   *
   * `MAX_SIZE_BITS`: log2 of the (exclusive) upper bound on managed
   * block sizes. Blocks that reach this size overflow and are
   * returned to the caller.
   */
  template<typename Rep, size_t MIN_SIZE_BITS, size_t MAX_SIZE_BITS>
  class Arena
  {
    static_assert(MAX_SIZE_BITS > MIN_SIZE_BITS);
    static_assert(MAX_SIZE_BITS < bits::BITS);
    static_assert(MIN_SIZE_BITS < bits::BITS);

    static constexpr size_t UNIT_SIZE = size_t(1) << MIN_SIZE_BITS;
    static constexpr size_t TWO_UNITS = size_t(2) << MIN_SIZE_BITS;

    static constexpr size_t B = 2;
    using Bins = ArenaBins<B, MIN_SIZE_BITS>;

    static_assert(
      bits::one_at_bit(MAX_SIZE_BITS) - 1 <= Bins::max_supported_size());

    using BinRep = typename Rep::BinRep;
    using RangeRep = typename Rep::RangeRep;

    using BinTree = RBTree<BinRep>;
    using RangeTree = RBTree<RangeRep>;

    stl::Array<BinTree, Bins::Bitmap::TOTAL_BINS> bin_trees{};
    RangeTree range_tree{};
    typename Bins::Bitmap bitmap{};

    // ---- Metadata helpers ----

    static ArenaVariant variant_of(size_t size, uintptr_t addr)
    {
      if (size == UNIT_SIZE)
        return ArenaVariant::Min;
      if (size == TWO_UNITS)
        return ((addr >> MIN_SIZE_BITS) & 1) == 0 ? ArenaVariant::EvenTwo :
                                                    ArenaVariant::OddTwo;
      return ArenaVariant::Large;
    }

    static stl::Pair<uintptr_t, size_t> range_from_addr(uintptr_t a)
    {
      if (a == 0)
        return {0, 0};
      auto v = Rep::get_variant(a);
      switch (v)
      {
        case ArenaVariant::Min:
          return {a, UNIT_SIZE};
        case ArenaVariant::EvenTwo:
        case ArenaVariant::OddTwo:
          return {a, TWO_UNITS};
        case ArenaVariant::Large:
        {
          size_t s = Rep::get_large_size(a);
          SNMALLOC_ASSERT(
            s > TWO_UNITS && s < bits::one_at_bit(MAX_SIZE_BITS) &&
            bits::align_down(s, UNIT_SIZE) == s);
          return {a, s};
        }
      }
      SNMALLOC_ASSERT(false);
      return {0, 0};
    }

    bool contains_min(uintptr_t a)
    {
      auto path = bin_trees[0].get_root_path();
      return bin_trees[0].find(path, a) &&
        Rep::get_variant(a) == ArenaVariant::Min;
    }

    void insert_block(uintptr_t addr, size_t size)
    {
      Rep::set_variant(addr, variant_of(size, addr));
      if (size > TWO_UNITS)
        Rep::set_large_size(addr, size);

      auto range = typename Bins::range_t{addr, size};
      size_t bin = bitmap.add(range);
      bin_trees[bin].insert_elem(addr);
      if (size >= TWO_UNITS)
        range_tree.insert_elem(addr);
    }

    void unlink_block(uintptr_t addr, size_t size)
    {
      auto range = typename Bins::range_t{addr, size};
      size_t bin = Bins::bin_index(range);
      bin_trees[bin].remove_elem(addr);
      if (size >= TWO_UNITS)
        range_tree.remove_elem(addr);
      if (bin_trees[bin].is_empty())
        bitmap.clear(bin);
    }

    friend struct ArenaTestAccess;

  public:
    using addr_t = uintptr_t;

    constexpr Arena() = default;

    /**
     * Add a free block at `addr` with `size` bytes. The block is
     * consolidated with any adjacent free neighbours. Returns
     * `{0, 0}` on success. If consolidation produces a block whose
     * size reaches `2^MAX_SIZE_BITS` bytes (the exclusive upper bound
     * on representable block sizes), the block is not inserted;
     * returns `{consolidated_addr, consolidated_size}` so the caller
     * can return it to a parent range.
     */
    stl::Pair<addr_t, size_t> add_block(addr_t addr, size_t size)
    {
      check_invariant();
      SNMALLOC_ASSERT(addr != 0);
      // Unit alignment is required: callers feeding parent ranges (e.g.
      // mmap-backed PalRange returns page-aligned but not chunk-aligned
      // memory) must trim their input to UNIT_SIZE before reaching here.
      // LargeArenaRange::add_range does this trim.
      SNMALLOC_ASSERT((addr & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT(size > 0);
      SNMALLOC_ASSERT((size & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT(size < bits::one_at_bit(MAX_SIZE_BITS));

      uintptr_t c_addr = addr;
      size_t c_size = size;

      auto merge = [&](uintptr_t n_addr, size_t n_size) {
        unlink_block(n_addr, n_size);
        if (n_addr < c_addr)
          c_addr = n_addr;
        c_size += n_size;
      };

      // Check range tree for non-min neighbours.
      auto [p_key, s_key] = range_tree.neighbours(addr);

      // Predecessor: check range tree, then fall back to min-size bin.
      auto [pa, ps] = range_from_addr(p_key);
      if (pa + ps == addr && Rep::can_consolidate(addr))
        merge(pa, ps);
      else if (
        addr >= UNIT_SIZE && Rep::can_consolidate(addr) &&
        contains_min(addr - UNIT_SIZE))
        merge(addr - UNIT_SIZE, UNIT_SIZE);

      // Successor: check range tree, then fall back to min-size bin.
      // `can_consolidate` reads succ_addr's pagemap entry. That entry is
      // only known to exist after a tree lookup confirms succ_addr is in
      // our region — succ_addr can be one past the registered range when
      // the input block ends at the high edge of the arena. Order the
      // checks so the tree check gates the pagemap read.
      auto [sa, ss] = range_from_addr(s_key);
      uintptr_t succ_addr = addr + size;
      if (sa == succ_addr && Rep::can_consolidate(succ_addr))
        merge(sa, ss);
      else if (
        succ_addr > addr && contains_min(succ_addr) &&
        Rep::can_consolidate(succ_addr))
        merge(succ_addr, UNIT_SIZE);

      // Arena-scale overflow: consolidated block spans the full arena.
      if (c_size >= bits::one_at_bit(MAX_SIZE_BITS))
        return {c_addr, c_size};

      // Insert consolidated block.
      insert_block(c_addr, c_size);

      check_invariant();
      return {0, 0};
    }

    /**
     * Remove exactly `size` bytes. Returns the address on success or
     * 0 if nothing fits. SC rounding is internal: the arena may
     * locate a larger free region but only the requested `size` is
     * handed out — the remainder rolls into the carve remainders
     * which are re-inserted via `add_block`.
     */
    addr_t remove_block(size_t size)
    {
      check_invariant();
      if (size == 0)
        return 0;

      if (size > Bins::max_supported_size())
        return 0;

      SNMALLOC_ASSERT((size & (UNIT_SIZE - 1)) == 0);

      size_t bin_id = bitmap.find_for_request(size);
      if (bin_id == SIZE_MAX)
        return 0;

      // remove_min returns the lowest-address entry (since compare
      // is k1 > k2). Read metadata after removal — remove_elem
      // does not clear node contents (redblacktree.h:535).
      uintptr_t block_addr = bin_trees[bin_id].remove_min();
      auto [_, block_size] = range_from_addr(block_addr);
      (void)_;

      if (block_size >= TWO_UNITS)
        range_tree.remove_elem(block_addr);

      if (bin_trees[bin_id].is_empty())
        bitmap.clear(bin_id);

      // Carve the requested size from the block.
      auto carved = Bins::carve({block_addr, block_size}, size);

      // Re-insert non-empty remainders. By the maximally-consolidated
      // invariant, these remainders have no adjacent free neighbours.
      if (carved.pre.size != 0)
      {
        insert_block(carved.pre.base, carved.pre.size);
      }

      if (carved.post.size != 0)
      {
        insert_block(carved.post.base, carved.post.size);
      }

      check_invariant();
      return carved.req.base;
    }

    /**
     * Structural invariant. Runs when `enabled` is true; defaults to
     * `Debug` so in-tree callers compile away in Release while tests
     * can opt in by passing `true` explicitly. Uses `SNMALLOC_CHECK`
     * rather than `SNMALLOC_ASSERT` so that test-driven invocations
     * are checked even under NDEBUG.
     *
     * Five clauses are verified:
     *  1. Maximally consolidated — no adjacent free blocks could be
     *     merged: (a) no two non-min range-tree entries touch across
     *     a consolidatable boundary, (b) no non-min entry touches a
     *     min entry, (c) no two min entries are adjacent.
     *  2. Cross-tree consistency — every range-tree entry appears in
     *     exactly one bin tree, and every non-min bin-tree entry
     *     appears in the range tree.
     *  3. Bin classification — every bin-tree entry sits in the bin
     *     its size selects.
     *  4. Bitmap consistency — the non-empty bin bit is set iff the
     *     corresponding bin tree has entries.
     *  5. Variant-tag consistency — each entry's pagemap variant tag
     *     matches the tag implied by its address and size, and Large
     *     variant entries carry the correct stored size.
     */
    void check_invariant(bool enabled = Debug)
    {
      if (!enabled)
        return;

      // 1a. No two adjacent non-min blocks (unless boundary prevents merge).
      {
        uintptr_t prev_addr = 0;
        size_t prev_size = 0;
        bool prev_valid = false;
        range_tree.for_each([&](uintptr_t node) {
          auto [a, s] = range_from_addr(node);
          if (prev_valid)
          {
            uintptr_t prev_end = prev_addr + prev_size;
            SNMALLOC_CHECK(prev_end != a || !Rep::can_consolidate(a));
          }
          prev_addr = a;
          prev_size = s;
          prev_valid = true;
        });
      }

      // 1b. No non-min block adjacent to a min block (unless boundary).
      range_tree.for_each([&](uintptr_t node) {
        auto [a, s] = range_from_addr(node);
        if (a >= UNIT_SIZE)
          SNMALLOC_CHECK(
            !contains_min(a - UNIT_SIZE) || !Rep::can_consolidate(a));
        uintptr_t end = a + s;
        SNMALLOC_CHECK(!contains_min(end) || !Rep::can_consolidate(end));
      });

      // 1c. No two adjacent min blocks (unless boundary).
      {
        uintptr_t prev = 0;
        bool prev_valid = false;
        bin_trees[0].for_each([&](uintptr_t node) {
          if (Rep::get_variant(node) != ArenaVariant::Min)
            return;
          if (prev_valid)
            SNMALLOC_CHECK(
              prev + UNIT_SIZE != node || !Rep::can_consolidate(node));
          prev = node;
          prev_valid = true;
        });
      }

      // 2. Cross-tree consistency.
      {
        size_t range_tree_count = 0;
        size_t bin_tree_nonmin_count = 0;

        for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
        {
          bin_trees[bin].for_each([&](uintptr_t node) {
            auto [a, s] = range_from_addr(node);
            if (s >= TWO_UNITS)
            {
              auto path = range_tree.get_root_path();
              SNMALLOC_CHECK(range_tree.find(path, node));
              bin_tree_nonmin_count++;
            }
          });
        }

        range_tree.for_each([&](uintptr_t node) {
          range_tree_count++;
          auto [a, s] = range_from_addr(node);
          auto range = typename Bins::range_t{a, s};
          size_t expected_bin = Bins::bin_index(range);
          auto path = bin_trees[expected_bin].get_root_path();
          SNMALLOC_CHECK(bin_trees[expected_bin].find(path, node));
        });

        SNMALLOC_CHECK(bin_tree_nonmin_count == range_tree_count);
      }

      // 3. Bin classification correctness.
      for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
      {
        bin_trees[bin].for_each([&](uintptr_t node) {
          auto [a, s] = range_from_addr(node);
          auto range = typename Bins::range_t{a, s};
          size_t expected_bin = Bins::bin_index(range);
          SNMALLOC_CHECK(expected_bin == bin);
        });
      }

      // 4. Bitmap consistency.
      for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
      {
        bool has_entries = !bin_trees[bin].is_empty();
        bool bit_set = bitmap.test(bin);
        SNMALLOC_CHECK(has_entries == bit_set);
      }

      // 5. Variant-tag consistency.
      for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
      {
        bin_trees[bin].for_each([&](uintptr_t node) {
          auto v = Rep::get_variant(node);
          auto [a, s] = range_from_addr(node);
          SNMALLOC_CHECK(v == variant_of(s, a));
          if (v == ArenaVariant::Large)
            SNMALLOC_CHECK(Rep::get_large_size(node) == s);
        });
      }
    }
  };
} // namespace snmalloc
