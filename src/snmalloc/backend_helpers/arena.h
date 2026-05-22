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
   *   - `get_large_size_chunks(addr)` / `set_large_size_chunks(addr, n)`
   *     — exact chunk count for `Large` blocks (3+ chunks).
   *   - `can_consolidate(higher_addr) -> bool` — whether the block at
   *     `higher_addr` may be merged with the block immediately below
   *     it. Returns false at allocation boundaries that must be
   *     preserved.
   *
   * `MIN_CHUNKS_BITS`: log2 of the minimum allocation unit in chunks
   * (currently only 0 is supported — 1-chunk minimum).
   *
   * `MAX_CHUNKS_BITS`: log2 of the arena size in chunks. Blocks that
   * reach this size overflow and are returned to the caller.
   */
  template<typename Rep, size_t MIN_CHUNKS_BITS, size_t MAX_CHUNKS_BITS>
  class Arena
  {
    static_assert(MIN_CHUNKS_BITS == 0, "Only MIN_CHUNKS_BITS == 0 supported");
    static_assert(MAX_CHUNKS_BITS > MIN_CHUNKS_BITS);
    static_assert(MAX_CHUNKS_BITS < bits::BITS);

    static constexpr size_t B = 2;
    using Bins = ArenaBins<B>;

    static_assert(
      bits::one_at_bit(MAX_CHUNKS_BITS) - 1 <= Bins::max_supported_chunks());

    using BinRep = typename Rep::BinRep;
    using RangeRep = typename Rep::RangeRep;

    using BinTree = RBTree<BinRep>;
    using RangeTree = RBTree<RangeRep>;

    stl::Array<BinTree, Bins::Bitmap::TOTAL_BINS> bin_trees{};
    RangeTree range_tree{};
    typename Bins::Bitmap bitmap{};

    // ---- Address-unit helpers ----

    static size_t addr_to_chunk(uintptr_t a)
    {
      return a >> MIN_CHUNK_BITS;
    }

    static uintptr_t chunk_to_addr(size_t c)
    {
      return static_cast<uintptr_t>(c) << MIN_CHUNK_BITS;
    }

    // ---- Metadata helpers ----

    static ArenaVariant
    variant_of(size_t size_chunks, size_t chunk_index)
    {
      if (size_chunks == 1)
        return ArenaVariant::Min;
      if (size_chunks == 2)
        return (chunk_index & 1) == 0 ? ArenaVariant::EvenTwo :
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
          return {a, 1};
        case ArenaVariant::EvenTwo:
        case ArenaVariant::OddTwo:
          return {a, 2};
        case ArenaVariant::Large:
          return {a, Rep::get_large_size_chunks(a)};
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

    void insert_block(uintptr_t addr, size_t size_chunks)
    {
      Rep::set_variant(addr, variant_of(size_chunks, addr_to_chunk(addr)));
      if (size_chunks >= 3)
        Rep::set_large_size_chunks(addr, size_chunks);

      auto chunk_range =
        typename Bins::range_t{addr_to_chunk(addr), size_chunks};
      size_t bin = bitmap.add(chunk_range);
      bin_trees[bin].insert_elem(addr);
      if (size_chunks >= 2)
        range_tree.insert_elem(addr);
    }

    void unlink_block(uintptr_t addr, size_t size_chunks)
    {
      auto chunk_range =
        typename Bins::range_t{addr_to_chunk(addr), size_chunks};
      size_t bin = bitmap.add(chunk_range);
      bin_trees[bin].remove_elem(addr);
      if (size_chunks >= 2)
        range_tree.remove_elem(addr);
      if (bin_trees[bin].is_empty())
        bitmap.clear(bin);
    }

    friend struct ArenaTestAccess;

  public:
    using addr_t = uintptr_t;

    constexpr Arena() = default;

    /**
     * Add a free block at `addr` with `size_chunks` chunks. The block
     * is consolidated with any adjacent free neighbours. Returns
     * `{0, 0}` on success. If consolidation produces a block spanning
     * the entire arena (`>= 2^MAX_CHUNKS_BITS` chunks), returns
     * `{consolidated_addr, consolidated_size}` and the arena is empty.
     */
    stl::Pair<addr_t, size_t> add_block(addr_t addr, size_t size_chunks)
    {
      check_invariant();
      SNMALLOC_ASSERT(addr != 0);
      SNMALLOC_ASSERT((addr & (MIN_CHUNK_SIZE - 1)) == 0);
      SNMALLOC_ASSERT(size_chunks > 0);
      SNMALLOC_ASSERT(size_chunks < bits::one_at_bit(MAX_CHUNKS_BITS));

      uintptr_t c_addr = addr;
      size_t c_size = size_chunks;

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
      if (pa + ps * MIN_CHUNK_SIZE == addr && Rep::can_consolidate(addr))
        merge(pa, ps);
      else if (
        addr >= MIN_CHUNK_SIZE && Rep::can_consolidate(addr) &&
        contains_min(addr - MIN_CHUNK_SIZE))
        merge(addr - MIN_CHUNK_SIZE, 1);

      // Successor: check range tree, then fall back to min-size bin.
      auto [sa, ss] = range_from_addr(s_key);
      uintptr_t succ_addr = addr + size_chunks * MIN_CHUNK_SIZE;
      if (sa == succ_addr && Rep::can_consolidate(succ_addr))
        merge(sa, ss);
      else if (
        succ_addr > addr && Rep::can_consolidate(succ_addr) &&
        contains_min(succ_addr))
        merge(succ_addr, 1);

      // Arena-scale overflow: consolidated block spans the full arena.
      if (c_size >= bits::one_at_bit(MAX_CHUNKS_BITS))
        return {c_addr, c_size};

      // Insert consolidated block.
      insert_block(c_addr, c_size);

      check_invariant();
      return {0, 0};
    }

    /**
     * Remove a block of at least `n_chunks` chunks. Returns
     * `{addr, actual_size}` on success, `{0, 0}` if nothing fits.
     * Any leftover from carving is re-inserted via `add_block`.
     */
    stl::Pair<addr_t, size_t> remove_block(size_t n_chunks)
    {
      check_invariant();
      if (n_chunks == 0)
        return {0, 0};

      if (n_chunks > Bins::max_supported_chunks())
        return {0, 0};

      size_t bin_id = bitmap.find_for_request(n_chunks);
      if (bin_id == SIZE_MAX)
        return {0, 0};

      // remove_min returns the lowest-address entry (since compare
      // is k1 > k2). Read metadata after removal — remove_elem
      // does not clear node contents (redblacktree.h:535).
      uintptr_t block_addr = bin_trees[bin_id].remove_min();
      auto [_, block_size] = range_from_addr(block_addr);
      (void)_;

      if (block_size >= 2)
        range_tree.remove_elem(block_addr);

      if (bin_trees[bin_id].is_empty())
        bitmap.clear(bin_id);

      // Carve the requested chunk count from the block.
      auto carved =
        Bins::carve({addr_to_chunk(block_addr), block_size}, n_chunks);

      // Re-insert non-empty remainders. By the maximally-consolidated
      // invariant, these remainders have no adjacent free neighbours.
      if (carved.pre.size != 0)
      {
        insert_block(chunk_to_addr(carved.pre.base), carved.pre.size);
      }

      if (carved.post.size != 0)
      {
        insert_block(chunk_to_addr(carved.post.base), carved.post.size);
      }

      check_invariant();
      return {chunk_to_addr(carved.req.base), carved.req.size};
    }

    /**
     * Structural invariant. Runs when `enabled` is true; defaults to
     * `Debug` so release tests can pass `true` explicitly.
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
            uintptr_t prev_end = prev_addr + prev_size * MIN_CHUNK_SIZE;
            SNMALLOC_ASSERT(prev_end != a || !Rep::can_consolidate(a));
          }
          prev_addr = a;
          prev_size = s;
          prev_valid = true;
        });
      }

      // 1b. No non-min block adjacent to a min block (unless boundary).
      range_tree.for_each([&](uintptr_t node) {
        auto [a, s] = range_from_addr(node);
        if (a >= MIN_CHUNK_SIZE)
          SNMALLOC_ASSERT(
            !contains_min(a - MIN_CHUNK_SIZE) || !Rep::can_consolidate(a));
        uintptr_t end = a + s * MIN_CHUNK_SIZE;
        SNMALLOC_ASSERT(!contains_min(end) || !Rep::can_consolidate(end));
      });

      // 1c. No two adjacent min blocks (unless boundary).
      {
        uintptr_t prev = 0;
        bool prev_valid = false;
        bin_trees[0].for_each([&](uintptr_t node) {
          if (Rep::get_variant(node) != ArenaVariant::Min)
            return;
          if (prev_valid)
            SNMALLOC_ASSERT(
              prev + MIN_CHUNK_SIZE != node || !Rep::can_consolidate(node));
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
            if (s >= 2)
            {
              auto path = range_tree.get_root_path();
              SNMALLOC_ASSERT(range_tree.find(path, node));
              bin_tree_nonmin_count++;
            }
          });
        }

        range_tree.for_each([&](uintptr_t node) {
          range_tree_count++;
          auto [a, s] = range_from_addr(node);
          auto chunk_range = typename Bins::range_t{addr_to_chunk(a), s};
          size_t expected_bin = Bins::bin_index(chunk_range);
          auto path = bin_trees[expected_bin].get_root_path();
          SNMALLOC_ASSERT(bin_trees[expected_bin].find(path, node));
        });

        SNMALLOC_ASSERT(bin_tree_nonmin_count == range_tree_count);
      }

      // 3. Bin classification correctness.
      for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
      {
        bin_trees[bin].for_each([&](uintptr_t node) {
          auto [a, s] = range_from_addr(node);
          auto chunk_range = typename Bins::range_t{addr_to_chunk(a), s};
          size_t expected_bin = Bins::bin_index(chunk_range);
          SNMALLOC_ASSERT(expected_bin == bin);
        });
      }

      // 4. Bitmap consistency.
      for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
      {
        bool has_entries = !bin_trees[bin].is_empty();
        bool bit_set = bitmap.test(bin);
        SNMALLOC_ASSERT(has_entries == bit_set);
      }

      // 5. Variant-tag consistency.
      for (size_t bin = 0; bin < Bins::Bitmap::TOTAL_BINS; bin++)
      {
        bin_trees[bin].for_each([&](uintptr_t node) {
          auto v = Rep::get_variant(node);
          auto [a, s] = range_from_addr(node);
          SNMALLOC_ASSERT(v == variant_of(s, addr_to_chunk(a)));
          if (v == ArenaVariant::Large)
            SNMALLOC_ASSERT(Rep::get_large_size_chunks(node) == s);
        });
      }
    }
  };
} // namespace snmalloc
