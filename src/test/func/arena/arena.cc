/**
 * Unit tests for Arena.
 *
 * Exercises the Rep adapters (BinRep, RangeRep), RBTree integration,
 * add_block with consolidation, remove_block with carving, the
 * five-clause invariant, and a randomised stress test with oracle.
 */

#include "test/setup.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif
#include "test/snmalloc_testlib.h"

#include <snmalloc/backend_helpers/arena.h>

namespace snmalloc
{
  // ---- MockRep: array-backed storage for testing ----

  // Each chunk-aligned address maps to a mock_entry via its chunk index.
  // word1/word2 hold bin-tree children; range_word1/range_word2 hold
  // range-tree children. variant and large_size_chunks hold metadata.
  struct mock_entry
  {
    uintptr_t word1{0};
    uintptr_t word2{0};
    uintptr_t range_word1{0};
    uintptr_t range_word2{0};
    ArenaVariant variant{ArenaVariant::Min};
    size_t large_size_chunks{0};
  };

  // Size the array for the largest test arena + trailing room.
  static constexpr size_t MOCK_ARENA_CHUNKS = 1024;
  static mock_entry mock_store[MOCK_ARENA_CHUNKS];

  static void reset_mock_store()
  {
    for (size_t i = 0; i < MOCK_ARENA_CHUNKS; i++)
      mock_store[i] = mock_entry{};
  }

  static size_t mock_index(uintptr_t addr)
  {
    size_t idx = addr >> MIN_CHUNK_BITS;
    SNMALLOC_ASSERT(idx < MOCK_ARENA_CHUNKS);
    SNMALLOC_ASSUME(idx < MOCK_ARENA_CHUNKS);
    return idx;
  }

  struct MockRep
  {
    static ArenaVariant get_variant(uintptr_t addr)
    {
      return mock_store[mock_index(addr)].variant;
    }

    static void set_variant(uintptr_t addr, ArenaVariant v)
    {
      mock_store[mock_index(addr)].variant = v;
    }

    static uintptr_t* ref_word(bool direction, uintptr_t addr)
    {
      auto& e = mock_store[mock_index(addr)];
      return direction ? &e.word1 : &e.word2;
    }

    static uintptr_t* ref_range_word(bool direction, uintptr_t addr)
    {
      auto& e = mock_store[mock_index(addr)];
      return direction ? &e.range_word1 : &e.range_word2;
    }

    static size_t get_large_size_chunks(uintptr_t addr)
    {
      return mock_store[mock_index(addr)].large_size_chunks;
    }

    static void set_large_size_chunks(uintptr_t addr, size_t s)
    {
      mock_store[mock_index(addr)].large_size_chunks = s;
    }
  };

  // ---- Test access ----
  struct ArenaTestAccess
  {
    template<typename Arena>
    static auto& get_bin_trees(Arena& a)
    {
      return a.bin_trees;
    }

    template<typename Arena>
    static auto& get_range_tree(Arena& a)
    {
      return a.range_tree;
    }

    template<typename Arena>
    static auto& get_bitmap(Arena& a)
    {
      return a.bitmap;
    }
  };

  // Convenience: chunk-aligned address from chunk index.
  static uintptr_t chunk_addr(size_t chunk_idx)
  {
    return static_cast<uintptr_t>(chunk_idx) << MIN_CHUNK_BITS;
  }

  // ---- Test types ----
  // K=6 → arena of 64 chunks, K=8 → 256 chunks, K=10 → 1024 chunks.
  template<size_t K>
  using TestArena = Arena<MockRep, 0, K>;

  using Bins = ArenaBins<2>;

  // ==================================================================
  // (A) Accessor round-trips
  // ==================================================================
  static void test_variant_roundtrip()
  {
    reset_mock_store();
    uintptr_t a = chunk_addr(10);

    MockRep::set_variant(a, ArenaVariant::Min);
    SNMALLOC_ASSERT(MockRep::get_variant(a) == ArenaVariant::Min);

    MockRep::set_variant(a, ArenaVariant::EvenTwo);
    SNMALLOC_ASSERT(MockRep::get_variant(a) == ArenaVariant::EvenTwo);

    MockRep::set_variant(a, ArenaVariant::Large);
    SNMALLOC_ASSERT(MockRep::get_variant(a) == ArenaVariant::Large);

    printf("  Variant round-trip: OK\n");
  }

  static void test_large_size_roundtrip()
  {
    reset_mock_store();
    uintptr_t a = chunk_addr(20);

    for (size_t s :
         {size_t{3},
          size_t{7},
          size_t{15},
          size_t{63},
          size_t{255},
          size_t{1000}})
    {
      MockRep::set_large_size_chunks(a, s);
      SNMALLOC_ASSERT(MockRep::get_large_size_chunks(a) == s);
    }

    printf("  Large-size round-trip: OK\n");
  }

  static void test_word_roundtrip()
  {
    reset_mock_store();
    uintptr_t a = chunk_addr(5);

    uintptr_t v1 = chunk_addr(10);
    uintptr_t v2 = chunk_addr(20);

    *MockRep::ref_word(true, a) = v1;
    *MockRep::ref_word(false, a) = v2;
    SNMALLOC_ASSERT(*MockRep::ref_word(true, a) == v1);
    SNMALLOC_ASSERT(*MockRep::ref_word(false, a) == v2);

    *MockRep::ref_range_word(true, a) = v2;
    *MockRep::ref_range_word(false, a) = v1;
    SNMALLOC_ASSERT(*MockRep::ref_range_word(true, a) == v2);
    SNMALLOC_ASSERT(*MockRep::ref_range_word(false, a) == v1);

    printf("  Word round-trip: OK\n");
  }

  // ==================================================================
  // (B) RBTree<BinRep> / RBTree<RangeRep> smoke
  // ==================================================================

  // We can't directly instantiate BinRep/RangeRep outside Arena
  // since they are private nested types. Instead, test them through
  // Arena's add_block/remove_block which exercise both trees.
  // For smoke testing of tree operations directly, we test through
  // the Arena's own invariant and operation correctness.

  static void test_rbtree_smoke_via_arena()
  {
    reset_mock_store();
    TestArena<8> arena;
    arena.check_invariant(true);

    // Insert a few non-adjacent blocks.
    uintptr_t a1 = chunk_addr(10);
    uintptr_t a2 = chunk_addr(20);
    uintptr_t a3 = chunk_addr(30);

    arena.add_block(a1, 3);
    arena.check_invariant(true);

    arena.add_block(a2, 5);
    arena.check_invariant(true);

    arena.add_block(a3, 1);
    arena.check_invariant(true);

    // Remove them.
    auto r1 = arena.remove_block(1);
    SNMALLOC_ASSERT(r1.first != 0);
    UNUSED(r1);
    arena.check_invariant(true);

    auto r2 = arena.remove_block(3);
    SNMALLOC_ASSERT(r2.first != 0);
    UNUSED(r2);
    arena.check_invariant(true);

    auto r3 = arena.remove_block(5);
    SNMALLOC_ASSERT(r3.first != 0);
    UNUSED(r3);
    arena.check_invariant(true);

    printf("  RBTree smoke via arena: OK\n");
  }

  // ==================================================================
  // (C) Empty-state invariant
  // ==================================================================
  template<size_t K>
  static void test_empty_invariant()
  {
    reset_mock_store();
    TestArena<K> arena;
    arena.check_invariant(true);
    printf("  Empty invariant (K=%zu): OK\n", K);
  }

  // ==================================================================
  // (D) add_block without consolidation
  // ==================================================================
  static void test_add_no_consolidation()
  {
    reset_mock_store();
    TestArena<8> arena;

    // Insert several non-adjacent blocks of various sizes.
    struct
    {
      size_t chunk_idx;
      size_t size;
    } blocks[] = {
      {10, 1},
      {20, 2},
      {30, 3},
      {40, 5},
      {50, 9},
    };

    for (auto& b : blocks)
    {
      auto result = arena.add_block(chunk_addr(b.chunk_idx), b.size);
      SNMALLOC_ASSERT(result.first == 0 && result.second == 0);
      UNUSED(result);
      arena.check_invariant(true);
    }

    printf("  add_block without consolidation: OK\n");
  }

  // ==================================================================
  // (E) remove_block exact-class + carving
  // ==================================================================
  static void test_remove_exact()
  {
    reset_mock_store();
    TestArena<8> arena;

    // Insert 3 blocks of size 5 at non-adjacent locations.
    arena.add_block(chunk_addr(10), 5);
    arena.add_block(chunk_addr(20), 5);
    arena.add_block(chunk_addr(30), 5);
    arena.check_invariant(true);

    // Remove 3 exact-size blocks.
    for (int i = 0; i < 3; i++)
    {
      auto r = arena.remove_block(5);
      SNMALLOC_ASSERT(r.first != 0);
      SNMALLOC_ASSERT(r.second == 5);
      UNUSED(r);
      arena.check_invariant(true);
    }

    // Arena should be empty now.
    auto r = arena.remove_block(1);
    SNMALLOC_ASSERT(r.first == 0);
    UNUSED(r);

    printf("  remove_block exact: OK\n");
  }

  static void test_remove_carving()
  {
    reset_mock_store();
    TestArena<8> arena;

    // Insert one block of size 10.
    arena.add_block(chunk_addr(10), 10);
    arena.check_invariant(true);

    // Request size 3 — should carve from the 10-chunk block.
    auto r = arena.remove_block(3);
    SNMALLOC_ASSERT(r.first != 0);
    // The carved piece should be exactly what Bins::carve produces.
    auto carved = Bins::carve({10, 10}, 3);
    SNMALLOC_ASSERT(r.second == carved.req.size);
    UNUSED(r);
    arena.check_invariant(true);

    // The remainders should still be in the arena.
    // We can try to remove everything that's left.
    size_t remaining = 10 - carved.req.size;
    while (remaining > 0)
    {
      auto r2 = arena.remove_block(1);
      SNMALLOC_ASSERT(r2.first != 0);
      arena.check_invariant(true);
      remaining -= r2.second;
    }

    // Should be empty.
    auto r3 = arena.remove_block(1);
    SNMALLOC_ASSERT(r3.first == 0);
    UNUSED(r3);

    printf("  remove_block carving: OK\n");
  }

  // ==================================================================
  // (F) Consolidation case matrix
  // ==================================================================

  // Helper: insert a block, verify invariant, return nothing.
  template<size_t K>
  static void
  add_and_check(TestArena<K>& arena, size_t chunk_idx, size_t size_chunks)
  {
    auto result = arena.add_block(chunk_addr(chunk_idx), size_chunks);
    SNMALLOC_ASSERT(result.first == 0 && result.second == 0);
    UNUSED(result);
    arena.check_invariant(true);
  }

  // Drain the arena by removing 1-chunk blocks until empty.
  // Returns the total chunks removed.
  template<size_t K>
  static size_t drain_arena(TestArena<K>& arena)
  {
    size_t total = 0;
    while (true)
    {
      auto r = arena.remove_block(1);
      if (r.first == 0)
        break;
      total += r.second;
      arena.check_invariant(true);
    }
    return total;
  }

  // Case 12: P-only, P min (size 1).
  static void test_consolidation_p_min()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 10, 1);
    add_and_check(arena, 11, 3);

    // Should have consolidated into a single 4-chunk block.
    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 4);
    UNUSED(total);

    printf("  Consolidation P-only, P min: OK\n");
  }

  // Case 13: P-only, P non-min.
  static void test_consolidation_p_nonmin()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 10, 3);
    add_and_check(arena, 13, 2);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 5);
    UNUSED(total);

    printf("  Consolidation P-only, P non-min: OK\n");
  }

  // Case 14: S-only, S min.
  static void test_consolidation_s_min()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 14, 1);
    add_and_check(arena, 11, 3);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 4);
    UNUSED(total);

    printf("  Consolidation S-only, S min: OK\n");
  }

  // Case 15: S-only, S non-min.
  static void test_consolidation_s_nonmin()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 14, 4);
    add_and_check(arena, 11, 3);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 7);
    UNUSED(total);

    printf("  Consolidation S-only, S non-min: OK\n");
  }

  // Case 16: P+S, both min.
  static void test_consolidation_ps_both_min()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 10, 1);
    add_and_check(arena, 12, 1);
    add_and_check(arena, 11, 1);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 3);
    UNUSED(total);

    printf("  Consolidation P+S, both min: OK\n");
  }

  // Case 17: P+S, P min, S non-min.
  static void test_consolidation_ps_p_min_s_nonmin()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 10, 1);
    add_and_check(arena, 14, 3);
    add_and_check(arena, 11, 3);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 7);
    UNUSED(total);

    printf("  Consolidation P+S, P min, S non-min: OK\n");
  }

  // Case 18: P+S, P non-min, S min.
  static void test_consolidation_ps_p_nonmin_s_min()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 10, 3);
    add_and_check(arena, 16, 1);
    add_and_check(arena, 13, 3);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 7);
    UNUSED(total);

    printf("  Consolidation P+S, P non-min, S min: OK\n");
  }

  // Case 19: P+S, both non-min.
  static void test_consolidation_ps_both_nonmin()
  {
    reset_mock_store();
    TestArena<8> arena;
    add_and_check(arena, 10, 4);
    add_and_check(arena, 19, 5);
    add_and_check(arena, 14, 5);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 14);
    UNUSED(total);

    printf("  Consolidation P+S, both non-min: OK\n");
  }

  // ==================================================================
  // (F2) OddTwo — unaligned size-2 blocks
  // ==================================================================

  static void test_oddtwo_variant()
  {
    // Odd chunk index → OddTwo, even → EvenTwo.
    reset_mock_store();
    TestArena<8> arena;

    // Odd address: chunk 11, size 2
    arena.add_block(chunk_addr(11), 2);
    SNMALLOC_ASSERT(
      MockRep::get_variant(chunk_addr(11)) == ArenaVariant::OddTwo);
    arena.check_invariant(true);

    // Even address: chunk 20, size 2
    arena.add_block(chunk_addr(20), 2);
    SNMALLOC_ASSERT(
      MockRep::get_variant(chunk_addr(20)) == ArenaVariant::EvenTwo);
    arena.check_invariant(true);

    // Both should be in the range tree.
    auto& rt = ArenaTestAccess::get_range_tree(arena);
    auto p1 = rt.get_root_path();
    SNMALLOC_ASSERT(rt.find(p1, chunk_addr(11)));
    auto p2 = rt.get_root_path();
    SNMALLOC_ASSERT(rt.find(p2, chunk_addr(20)));

    // OddTwo (chunk 11) should be in bin 0 (size-1 servable set).
    auto& bt0 = ArenaTestAccess::get_bin_trees(arena)[0];
    auto p3 = bt0.get_root_path();
    SNMALLOC_ASSERT(bt0.find(p3, chunk_addr(11)));
    UNUSED(p1, p2, p3);

    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 4);
    UNUSED(total);

    printf("  OddTwo variant tagging: OK\n");
  }

  static void test_oddtwo_contains_min_filter()
  {
    // contains_min must not match OddTwo entries.
    reset_mock_store();
    TestArena<8> arena;

    // Add OddTwo block at chunk 11 (odd, size 2).
    arena.add_block(chunk_addr(11), 2);
    arena.check_invariant(true);

    // Add a size-1 block at chunk 14, non-adjacent.
    arena.add_block(chunk_addr(14), 1);
    arena.check_invariant(true);

    // Now add chunk 13 (size 1). Its successor check should NOT
    // pick up chunk 11's OddTwo entry via contains_min. It should
    // just insert as size 1.
    arena.add_block(chunk_addr(13), 1);
    arena.check_invariant(true);

    // Chunk 13 should consolidate with chunk 14 (min successor),
    // but NOT with chunk 11's OddTwo (range tree handles that).
    // Drain to verify total.
    size_t total = drain_arena(arena);
    SNMALLOC_ASSERT(total == 4);
    UNUSED(total);

    printf("  OddTwo contains_min filter: OK\n");
  }

  static void test_oddtwo_consolidation()
  {
    // OddTwo block should consolidate via the range tree.
    reset_mock_store();
    TestArena<8> arena;

    // Add OddTwo at chunk 11 (odd, size 2 → chunks 11-12).
    arena.add_block(chunk_addr(11), 2);
    arena.check_invariant(true);

    // Add adjacent block at chunk 13 (size 1).
    // Range tree finds OddTwo at 11 as predecessor? No — chunk 13's
    // predecessor in range tree is chunk 11 (size 2, ends at 13).
    // So they should consolidate into size 3 at chunk 11.
    arena.add_block(chunk_addr(13), 1);
    arena.check_invariant(true);

    auto r = arena.remove_block(3);
    SNMALLOC_ASSERT(r.first == chunk_addr(11));
    SNMALLOC_ASSERT(r.second == 3);
    UNUSED(r);

    printf("  OddTwo consolidation (successor): OK\n");
  }

  static void test_oddtwo_consolidation_pred()
  {
    // Consolidation where the new block is a predecessor of OddTwo.
    reset_mock_store();
    TestArena<8> arena;

    // Add OddTwo at chunk 11 (odd, size 2 → chunks 11-12).
    arena.add_block(chunk_addr(11), 2);
    arena.check_invariant(true);

    // Add block at chunk 10 (size 1). OddTwo at 11 is the successor
    // in the range tree → consolidate into size 3 at chunk 10.
    arena.add_block(chunk_addr(10), 1);
    arena.check_invariant(true);

    auto r = arena.remove_block(3);
    SNMALLOC_ASSERT(r.first == chunk_addr(10));
    SNMALLOC_ASSERT(r.second == 3);
    UNUSED(r);

    printf("  OddTwo consolidation (predecessor): OK\n");
  }

  static void test_oddtwo_remove_carve()
  {
    // remove_block(1) from an OddTwo block should carve correctly.
    reset_mock_store();
    TestArena<8> arena;

    // Add OddTwo at chunk 11 (odd, size 2).
    arena.add_block(chunk_addr(11), 2);
    arena.check_invariant(true);

    // Remove 1 chunk. Should carve from the OddTwo block.
    auto r = arena.remove_block(1);
    SNMALLOC_ASSERT(r.first != 0);
    SNMALLOC_ASSERT(r.second == 1);
    arena.check_invariant(true);

    // The remainder (1 chunk) should be Min variant.
    auto r2 = arena.remove_block(1);
    SNMALLOC_ASSERT(r2.first != 0);
    SNMALLOC_ASSERT(r2.second == 1);
    UNUSED(r, r2);

    // Arena should be empty now.
    auto r3 = arena.remove_block(1);
    SNMALLOC_ASSERT(r3.first == 0);
    UNUSED(r3);

    printf("  OddTwo remove + carve: OK\n");
  }

  // ==================================================================
  // (G) Overflow — arena-scale consolidation
  // ==================================================================
  static void test_overflow()
  {
    // K=4 → 16-chunk arena. Use base offset 16 to avoid address 0.
    reset_mock_store();
    TestArena<4> arena;

    constexpr size_t BASE = 16;

    // Step 1: add even-indexed chunks as individual blocks (8 blocks).
    for (size_t i = 0; i < 16; i += 2)
    {
      arena.add_block(chunk_addr(BASE + i), 1);
      arena.check_invariant(true);
    }

    // Step 2: fill odd-indexed gaps. Each add consolidates with its
    // even-indexed neighbours. The last add completes the arena.
    for (size_t i = 1; i < 16; i += 2)
    {
      arena.add_block(chunk_addr(BASE + i), 1);
      // Don't check invariant on the last add — it returns overflow.
      if (i < 15)
      {
        arena.check_invariant(true);
      }
    }

    // The last add should have triggered overflow (16 chunks = 2^4).
    auto r = arena.remove_block(1);
    SNMALLOC_ASSERT(r.first == 0);
    UNUSED(r);

    printf("  Overflow (arena-scale consolidation): OK\n");
  }

  static void test_overflow_precise()
  {
    // K=4 → 16-chunk arena. Use base offset 16 to avoid address 0.
    reset_mock_store();
    TestArena<4> arena;

    constexpr size_t BASE = 16;

    arena.add_block(chunk_addr(BASE), 8);
    arena.check_invariant(true);

    // Adding [BASE+8, BASE+16) consolidates to 16 chunks = 2^4 → overflow.
    auto r = arena.add_block(chunk_addr(BASE + 8), 8);
    SNMALLOC_ASSERT(r.first == chunk_addr(BASE));
    SNMALLOC_ASSERT(r.second == 16);
    UNUSED(r);

    auto r2 = arena.remove_block(1);
    SNMALLOC_ASSERT(r2.first == 0);
    UNUSED(r2);

    printf("  Overflow precise: OK\n");
  }

  // ==================================================================
  // (H) Randomised stress with oracle
  // ==================================================================

  // Oracle: std::set of (addr_chunks, size_chunks) representing
  // maximally-consolidated free set.
  struct OracleRange
  {
    size_t addr; // in chunk units
    size_t size; // in chunk units

    bool operator<(const OracleRange& o) const
    {
      return addr < o.addr;
    }

    bool operator==(const OracleRange& o) const
    {
      return addr == o.addr && size == o.size;
    }
  };

  class Oracle
  {
    std::set<OracleRange> ranges;
    size_t base_offset; // chunk offset to match arena addresses

  public:
    Oracle() : base_offset(0) {}

    Oracle(size_t base) : base_offset(base) {}

    void add(size_t addr_chunks, size_t size_chunks)
    {
      OracleRange key{addr_chunks, size_chunks};
      auto it = ranges.lower_bound(key);

      size_t new_addr = addr_chunks;
      size_t new_size = size_chunks;

      if (it != ranges.end() && it->addr == new_addr + new_size)
      {
        new_size += it->size;
        it = ranges.erase(it);
      }

      if (it != ranges.begin())
      {
        auto prev = std::prev(it);
        if (prev->addr + prev->size == new_addr)
        {
          new_addr = prev->addr;
          new_size += prev->size;
          ranges.erase(prev);
        }
      }

      ranges.insert({new_addr, new_size});
    }

    // Returns {addr_chunks, size_chunks} or {0, 0} if nothing fits.
    // addr_chunks is oracle-relative (without base offset).
    std::pair<size_t, size_t> remove(size_t n_chunks)
    {
      if (n_chunks == 0 || n_chunks > Bins::max_supported_chunks())
        return {0, 0};

      // Mirror the arena exactly: build a bitmap using arena-offset
      // addresses (so bin classification matches), then find_for_request.
      typename Bins::Bitmap bm{};
      std::map<size_t, std::vector<std::set<OracleRange>::iterator>> by_bin;

      for (auto it = ranges.begin(); it != ranges.end(); ++it)
      {
        // Use base-offset address for bin classification.
        Bins::range_t r{base_offset + it->addr, it->size};
        size_t bin = bm.add(r);
        by_bin[bin].push_back(it);
      }

      size_t bin_id = bm.find_for_request(n_chunks);
      if (bin_id == SIZE_MAX)
        return {0, 0};

      auto& entries = by_bin[bin_id];
      auto best_it = entries[0];
      for (size_t i = 1; i < entries.size(); i++)
      {
        if (entries[i]->addr < best_it->addr)
          best_it = entries[i];
      }

      OracleRange block = *best_it;
      ranges.erase(best_it);

      // Carve using base-offset address.
      auto carved =
        Bins::carve({base_offset + block.addr, block.size}, n_chunks);
      if (carved.pre.size != 0)
        ranges.insert({carved.pre.base - base_offset, carved.pre.size});
      if (carved.post.size != 0)
        ranges.insert({carved.post.base - base_offset, carved.post.size});

      return {carved.req.base - base_offset, carved.req.size};
    }

    bool empty() const
    {
      return ranges.empty();
    }

    size_t count() const
    {
      return ranges.size();
    }
  };

  template<size_t K>
  static void test_stress_seed(size_t seed, size_t num_ops)
  {
    reset_mock_store();
    TestArena<K> arena;

    constexpr size_t ARENA_CHUNKS = bits::one_at_bit(K);
    // Offset all chunk addresses to avoid address 0 (tree null).
    constexpr size_t BASE = ARENA_CHUNKS;
    Oracle oracle(BASE);
    // Track which chunks are allocated (not free).
    std::vector<bool> allocated(ARENA_CHUNKS, true);

    xoroshiro::p128r64 rng(seed);

    for (size_t op = 0; op < num_ops; op++)
    {
      bool do_add = (rng.next() % 3) != 0; // Bias towards adding.

      if (do_add)
      {
        // Find a free address range of random size within the arena.
        size_t max_size = ARENA_CHUNKS / 4;
        if (max_size < 1)
          max_size = 1;
        size_t size = (rng.next() % max_size) + 1;
        size_t start = rng.next() % ARENA_CHUNKS;

        // Adjust: find a contiguous allocated (not free) region.
        // We need a region that's currently allocated (not in the
        // free set) to add back.
        bool found = false;
        for (size_t try_start = start; try_start < ARENA_CHUNKS; try_start++)
        {
          // Check if [try_start, try_start + size) is all allocated.
          size_t actual_size = 0;
          for (size_t j = try_start; j < ARENA_CHUNKS && j < try_start + size;
               j++)
          {
            if (!allocated[j])
              break;
            actual_size++;
          }

          if (actual_size >= 1)
          {
            size = actual_size;
            start = try_start;
            found = true;
            break;
          }
        }

        if (!found)
          continue;

        // Clamp to arena size limit.
        if (size >= ARENA_CHUNKS)
          size = ARENA_CHUNKS - 1;
        if (start + size > ARENA_CHUNKS)
          size = ARENA_CHUNKS - start;
        if (size == 0)
          continue;

        // Mark as free.
        SNMALLOC_ASSERT(start + size <= ARENA_CHUNKS);
        for (size_t j = start; j < start + size; j++)
          allocated[j] = false;

        auto result = arena.add_block(chunk_addr(BASE + start), size);
        oracle.add(start, size);

        if (result.first != 0)
        {
          // Overflow — all chunks are now free and returned to caller.
          // Oracle should be empty after we remove the overflow range.
          // Reset: mark everything as allocated again, clear oracle.
          for (size_t j = 0; j < ARENA_CHUNKS; j++)
            allocated[j] = true;
          oracle = Oracle(BASE);
          // The overflow range isn't tracked by the arena anymore.
        }

        arena.check_invariant(true);
      }
      else
      {
        // Remove.
        size_t max_req = ARENA_CHUNKS / 4;
        if (max_req < 1)
          max_req = 1;
        size_t n = (rng.next() % max_req) + 1;

        auto arena_result = arena.remove_block(n);
        auto oracle_result = oracle.remove(n);
        UNUSED(arena_result);

        // Both should agree on success/failure.
        // Use size == 0 to detect failure, since oracle address 0 is valid.
        if (oracle_result.second == 0)
        {
          SNMALLOC_ASSERT(arena_result.second == 0);
        }
        else
        {
          SNMALLOC_ASSERT(arena_result.second != 0);
          // Both should return the same address and size.
          SNMALLOC_ASSERT(
            arena_result.first == chunk_addr(BASE + oracle_result.first));
          SNMALLOC_ASSERT(arena_result.second == oracle_result.second);

          // Mark as allocated.
          size_t start = oracle_result.first;
          SNMALLOC_ASSERT(start + oracle_result.second <= ARENA_CHUNKS);
          for (size_t j = start; j < start + oracle_result.second; j++)
            allocated[j] = true;
        }

        arena.check_invariant(true);
      }
    }
  }

  static void test_stress()
  {
    constexpr size_t K = 6; // 64-chunk arena
    constexpr size_t NUM_OPS = 500;
    constexpr size_t NUM_SEEDS = 50;

    for (size_t seed = 1; seed <= NUM_SEEDS; seed++)
    {
      test_stress_seed<K>(seed, NUM_OPS);
    }
    printf(
      "  Randomised stress (%zu seeds x %zu ops): OK\n", NUM_SEEDS, NUM_OPS);
  }

  // ==================================================================
  // (I) Multi-instance: shared pagemap, blocks migrating between arenas
  // ==================================================================

  static void test_multi_instance_basic()
  {
    reset_mock_store();
    TestArena<8> arena_a;
    TestArena<8> arena_b;
    constexpr size_t BASE = 256; // avoid address 0

    // Add distinct blocks to each arena.
    arena_a.add_block(chunk_addr(BASE + 10), 5);
    arena_b.add_block(chunk_addr(BASE + 30), 5);
    arena_a.check_invariant(true);
    arena_b.check_invariant(true);

    // Migrate a block from A to B.
    auto [a_addr, a_size] = arena_a.remove_block(3);
    SNMALLOC_ASSERT(a_addr != 0 && a_size != 0);
    arena_a.check_invariant(true);

    arena_b.add_block(a_addr, a_size);
    arena_a.check_invariant(true);
    arena_b.check_invariant(true);

    // Migrate from B back to A.
    auto [b_addr, b_size] = arena_b.remove_block(2);
    SNMALLOC_ASSERT(b_addr != 0 && b_size != 0);
    arena_b.check_invariant(true);

    arena_a.add_block(b_addr, b_size);
    arena_a.check_invariant(true);
    arena_b.check_invariant(true);

    printf("  Basic migration: OK\n");
  }

  static void test_multi_instance_consolidation()
  {
    reset_mock_store();
    TestArena<8> arena_a;
    TestArena<8> arena_b;
    constexpr size_t BASE = 256;

    // Arena B holds two blocks with a gap: [20..24) and [28..32).
    arena_b.add_block(chunk_addr(BASE + 20), 4);
    arena_b.add_block(chunk_addr(BASE + 28), 4);
    arena_b.check_invariant(true);

    // Arena A holds the gap: [24..28).
    arena_a.add_block(chunk_addr(BASE + 24), 4);
    arena_a.check_invariant(true);

    // Migrate the gap from A to B → should consolidate into [20..32).
    auto [addr, size] = arena_a.remove_block(4);
    SNMALLOC_ASSERT(addr == chunk_addr(BASE + 24));
    SNMALLOC_ASSERT(size == 4);
    arena_a.check_invariant(true);

    arena_b.add_block(addr, size);
    arena_b.check_invariant(true);

    // B should now serve a size-12 request from the consolidated block.
    auto [r_addr, r_size] = arena_b.remove_block(12);
    SNMALLOC_ASSERT(r_addr == chunk_addr(BASE + 20));
    SNMALLOC_ASSERT(r_size == 12);
    arena_b.check_invariant(true);

    printf("  Consolidation after migration: OK\n");
  }

  template<size_t K>
  static void test_multi_stress_seed(size_t seed, size_t num_ops)
  {
    reset_mock_store();
    TestArena<K> arena_a;
    TestArena<K> arena_b;

    constexpr size_t ARENA_CHUNKS = bits::one_at_bit(K);
    constexpr size_t BASE = ARENA_CHUNKS;
    Oracle oracle_a(BASE);
    Oracle oracle_b(BASE);

    // 0 = not in any arena, 1 = in arena A, 2 = in arena B.
    std::vector<uint8_t> owner(ARENA_CHUNKS, 0);

    xoroshiro::p128r64 rng(seed);

    for (size_t op = 0; op < num_ops; op++)
    {
      // 0,1 = add to A or B; 2,3 = remove from A or B; 4 = migrate.
      size_t action = rng.next() % 5;

      bool target_a = (action & 1) == 0;
      auto& arena = target_a ? arena_a : arena_b;
      auto& oracle = target_a ? oracle_a : oracle_b;
      uint8_t my_id = target_a ? 1 : 2;

      if (action <= 1)
      {
        // Add: find a contiguous unowned region to free into this arena.
        size_t max_size = ARENA_CHUNKS / 4;
        if (max_size < 1)
          max_size = 1;
        size_t size = (rng.next() % max_size) + 1;
        size_t start = rng.next() % ARENA_CHUNKS;

        bool found = false;
        for (size_t s = start; s < ARENA_CHUNKS; s++)
        {
          size_t actual = 0;
          for (size_t j = s; j < ARENA_CHUNKS && j < s + size; j++)
          {
            if (owner[j] != 0)
              break;
            actual++;
          }
          if (actual >= 1)
          {
            size = actual;
            start = s;
            found = true;
            break;
          }
        }
        if (!found)
          continue;

        if (size >= ARENA_CHUNKS)
          size = ARENA_CHUNKS - 1;
        if (start + size > ARENA_CHUNKS)
          size = ARENA_CHUNKS - start;
        if (size == 0)
          continue;

        for (size_t j = start; j < start + size; j++)
          owner[j] = my_id;

        auto result = arena.add_block(chunk_addr(BASE + start), size);
        oracle.add(start, size);

        if (result.first != 0)
        {
          for (size_t j = 0; j < ARENA_CHUNKS; j++)
            if (owner[j] == my_id)
              owner[j] = 0;
          oracle = Oracle(BASE);
        }

        arena.check_invariant(true);
      }
      else if (action <= 3)
      {
        // Remove from this arena.
        size_t max_req = ARENA_CHUNKS / 4;
        if (max_req < 1)
          max_req = 1;
        size_t n = (rng.next() % max_req) + 1;

        auto arena_r = arena.remove_block(n);
        auto oracle_r = oracle.remove(n);
        UNUSED(arena_r);

        if (oracle_r.second == 0)
        {
          SNMALLOC_ASSERT(arena_r.second == 0);
        }
        else
        {
          SNMALLOC_ASSERT(arena_r.second != 0);
          SNMALLOC_ASSERT(arena_r.first == chunk_addr(BASE + oracle_r.first));
          SNMALLOC_ASSERT(arena_r.second == oracle_r.second);

          for (size_t j = oracle_r.first; j < oracle_r.first + oracle_r.second;
               j++)
          {
            SNMALLOC_ASSERT(owner[j] == my_id);
            owner[j] = 0;
          }
        }

        arena.check_invariant(true);
      }
      else
      {
        // Migrate: remove from one arena, add to the other.
        bool from_a = (rng.next() & 1) == 0;
        auto& src = from_a ? arena_a : arena_b;
        auto& src_oracle = from_a ? oracle_a : oracle_b;
        auto& dst = from_a ? arena_b : arena_a;
        auto& dst_oracle = from_a ? oracle_b : oracle_a;
        uint8_t src_id = from_a ? 1 : 2;
        uint8_t dst_id = from_a ? 2 : 1;
        UNUSED(src_id);

        size_t n = (rng.next() % 3) + 1;
        auto src_r = src.remove_block(n);
        auto src_or = src_oracle.remove(n);

        if (src_or.second == 0)
        {
          SNMALLOC_ASSERT(src_r.second == 0);
        }
        else
        {
          SNMALLOC_ASSERT(src_r.second != 0);
          SNMALLOC_ASSERT(src_r.first == chunk_addr(BASE + src_or.first));
          SNMALLOC_ASSERT(src_r.second == src_or.second);

          for (size_t j = src_or.first; j < src_or.first + src_or.second; j++)
          {
            SNMALLOC_ASSERT(owner[j] == src_id);
            owner[j] = dst_id;
          }

          auto dst_r = dst.add_block(src_r.first, src_r.second);
          dst_oracle.add(src_or.first, src_or.second);

          if (dst_r.first != 0)
          {
            for (size_t j = 0; j < ARENA_CHUNKS; j++)
              if (owner[j] == dst_id)
                owner[j] = 0;
            dst_oracle = Oracle(BASE);
          }
        }

        src.check_invariant(true);
        dst.check_invariant(true);
      }
    }
  }

  static void test_multi_stress()
  {
    constexpr size_t K = 6; // 64-chunk arena
    constexpr size_t NUM_OPS = 500;
    constexpr size_t NUM_SEEDS = 50;

    for (size_t seed = 1; seed <= NUM_SEEDS; seed++)
      test_multi_stress_seed<K>(seed, NUM_OPS);

    printf(
      "  Multi-instance stress (%zu seeds x %zu ops): OK\n",
      NUM_SEEDS,
      NUM_OPS);
  }

} // namespace snmalloc

int main()
{
  printf("--- Arena tests ---\n");

  printf("(A) Accessor round-trips:\n");
  snmalloc::test_variant_roundtrip();
  snmalloc::test_large_size_roundtrip();
  snmalloc::test_word_roundtrip();

  printf("(B) RBTree smoke via arena:\n");
  snmalloc::test_rbtree_smoke_via_arena();

  printf("(C) Empty-state invariant:\n");
  snmalloc::test_empty_invariant<4>();
  snmalloc::test_empty_invariant<5>();
  snmalloc::test_empty_invariant<6>();

  printf("(D) add_block without consolidation:\n");
  snmalloc::test_add_no_consolidation();

  printf("(E) remove_block:\n");
  snmalloc::test_remove_exact();
  snmalloc::test_remove_carving();

  printf("(F) Consolidation case matrix:\n");
  snmalloc::test_consolidation_p_min();
  snmalloc::test_consolidation_p_nonmin();
  snmalloc::test_consolidation_s_min();
  snmalloc::test_consolidation_s_nonmin();
  snmalloc::test_consolidation_ps_both_min();
  snmalloc::test_consolidation_ps_p_min_s_nonmin();
  snmalloc::test_consolidation_ps_p_nonmin_s_min();
  snmalloc::test_consolidation_ps_both_nonmin();

  printf("(F2) OddTwo (unaligned size-2):\n");
  snmalloc::test_oddtwo_variant();
  snmalloc::test_oddtwo_contains_min_filter();
  snmalloc::test_oddtwo_consolidation();
  snmalloc::test_oddtwo_consolidation_pred();
  snmalloc::test_oddtwo_remove_carve();

  printf("(G) Overflow:\n");
  snmalloc::test_overflow();
  snmalloc::test_overflow_precise();

  printf("(H) Randomised stress:\n");
  snmalloc::test_stress();

  printf("(I) Multi-instance:\n");
  snmalloc::test_multi_instance_basic();
  snmalloc::test_multi_instance_consolidation();
  snmalloc::test_multi_stress();

  printf("All Arena tests passed.\n");
  return 0;
}
