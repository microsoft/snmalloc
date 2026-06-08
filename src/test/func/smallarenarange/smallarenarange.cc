/**
 * Unit tests for `InplaceRep` exercised through `Arena`.
 *
 * Distinct from the `arena` test (which uses an array-backed
 * MockRep): here the Rep is the in-band representation,
 * and each free block's tree-node storage lives at the block's own
 * head bytes. The test allocates a single chunk-aligned backing
 * buffer and treats addresses within it as block bases.
 */

#include "test/setup.h"
#include "test/snmalloc_testlib.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <new>
#include <set>
#include <snmalloc/backend_helpers/arena.h>
#include <snmalloc/backend_helpers/authmap.h>
#include <snmalloc/backend_helpers/inplacerep.h>
#include <snmalloc/backend_helpers/smallarenarange.h>
#include <vector>

namespace snmalloc
{
  using Rep = InplaceRep<DummyAuthmap, capptr::bounds::Arena>;
  static constexpr size_t UNIT_SIZE = Rep::UNIT_SIZE;
  static constexpr size_t MIN_BITS = Rep::MIN_BITS;

  // Arena spans one chunk's worth of space (max block size =
  // MIN_CHUNK_SIZE - UNIT_SIZE, since the arena's MAX is exclusive).
  static constexpr size_t MAX_SIZE_BITS = MIN_CHUNK_BITS;
  using TestArena = Arena<Rep, MIN_BITS, MAX_SIZE_BITS>;

  // Backing buffer: must be UNIT_SIZE-aligned so block bases are
  // unit-aligned and the in-band node fields land at the expected
  // offsets. Sized to comfortably cover the arena's full range plus
  // a small base offset that keeps block addresses non-zero (zero
  // is the tree null sentinel). Oversized by MIN_CHUNK_SIZE so the
  // base can be aligned up at runtime — MSVC rejects alignas values
  // as large as MIN_CHUNK_SIZE on static storage.
  static unsigned char backing[3 * MIN_CHUNK_SIZE];

  static uintptr_t base_addr()
  {
    // Round up to MIN_CHUNK_SIZE, then offset by MIN_CHUNK_SIZE to
    // keep addresses well clear of zero.
    uintptr_t raw = reinterpret_cast<uintptr_t>(&backing[0]);
    uintptr_t aligned = (raw + MIN_CHUNK_SIZE - 1) & ~(MIN_CHUNK_SIZE - 1);
    return aligned + MIN_CHUNK_SIZE;
  }

  static void reset_backing()
  {
    for (size_t i = 0; i < sizeof(backing); i++)
      backing[i] = 0;
  }

  static uintptr_t unit_addr(size_t unit_idx)
  {
    return base_addr() + unit_idx * UNIT_SIZE;
  }

  static constexpr size_t unit_size(size_t n_units)
  {
    return n_units * UNIT_SIZE;
  }

  // ==================================================================
  // (A) Round-trip: variant tag and large-size storage survive
  // independent of bin/range pointer writes.
  // ==================================================================

  static void test_variant_roundtrip()
  {
    reset_backing();
    uintptr_t a = unit_addr(0);

    for (auto v :
         {ArenaVariant::Min,
          ArenaVariant::EvenTwo,
          ArenaVariant::OddTwo,
          ArenaVariant::Large})
    {
      Rep::set_variant(a, v);
      SNMALLOC_CHECK(Rep::get_variant(a) == v);
    }

    // Variant tag must not interfere with the red bit at bit 0.
    Rep::set_variant(a, ArenaVariant::OddTwo);
    Rep::BinRep::set_red(a, true);
    SNMALLOC_CHECK(Rep::BinRep::is_red(a));
    SNMALLOC_CHECK(Rep::get_variant(a) == ArenaVariant::OddTwo);

    Rep::BinRep::set_red(a, false);
    SNMALLOC_CHECK(!Rep::BinRep::is_red(a));
    SNMALLOC_CHECK(Rep::get_variant(a) == ArenaVariant::OddTwo);

    printf("  Variant + red roundtrip: OK\n");
  }

  static void test_large_size_roundtrip()
  {
    reset_backing();
    uintptr_t a = unit_addr(0);

    for (size_t s : {unit_size(3), unit_size(7), unit_size(17), unit_size(125)})
    {
      Rep::set_large_size(a, s);
      SNMALLOC_CHECK(Rep::get_large_size(a) == s);
    }

    printf("  Large-size roundtrip: OK\n");
  }

  // ==================================================================
  // (B) Bin-tree and range-tree red bits live in different units and
  // must not alias.
  // ==================================================================

  static void test_red_bits_independent()
  {
    reset_backing();
    uintptr_t a = unit_addr(0);

    Rep::BinRep::set_red(a, true);
    Rep::RangeRep::set_red(a, false);
    SNMALLOC_CHECK(Rep::BinRep::is_red(a));
    SNMALLOC_CHECK(!Rep::RangeRep::is_red(a));

    Rep::BinRep::set_red(a, false);
    Rep::RangeRep::set_red(a, true);
    SNMALLOC_CHECK(!Rep::BinRep::is_red(a));
    SNMALLOC_CHECK(Rep::RangeRep::is_red(a));

    printf("  Bin/range red bits independent: OK\n");
  }

  // ==================================================================
  // (B2) `can_consolidate` refuses chunk-boundary merges.
  // SmallArenaRange splits incoming ranges at chunk boundaries, but
  // adjacent intra-chunk fragments meeting at a boundary would
  // otherwise be merged by Arena. The predicate is what
  // prevents that.
  // ==================================================================

  static void test_can_consolidate_chunk_boundary()
  {
    // Chunk-aligned higher_addr means the lower neighbour ends at
    // a chunk boundary — refuse.
    SNMALLOC_CHECK(!Rep::can_consolidate(MIN_CHUNK_SIZE));
    SNMALLOC_CHECK(!Rep::can_consolidate(2 * MIN_CHUNK_SIZE));
    // Non-chunk-aligned higher_addr is fine to merge.
    SNMALLOC_CHECK(Rep::can_consolidate(MIN_CHUNK_SIZE + UNIT_SIZE));
    SNMALLOC_CHECK(Rep::can_consolidate(MIN_CHUNK_SIZE - UNIT_SIZE));
    SNMALLOC_CHECK(Rep::can_consolidate(UNIT_SIZE));

    printf("  can_consolidate chunk-boundary refuse: OK\n");
  }

  // ==================================================================
  // (C) Through the arena: add a single block and remove it.
  // ==================================================================

  static void test_arena_add_remove_single()
  {
    reset_backing();
    TestArena arena;
    arena.check_invariant(true);

    auto a = unit_addr(0);
    auto [ov_a, ov_s] = arena.add_block(a, unit_size(4));
    SNMALLOC_CHECK(ov_a == 0 && ov_s == 0);
    arena.check_invariant(true);

    auto got = arena.remove_block(unit_size(4));
    SNMALLOC_CHECK(got == a);
    arena.check_invariant(true);

    printf("  Arena add/remove single: OK\n");
  }

  // ==================================================================
  // (D) Consolidation across two adjacent free blocks.
  // ==================================================================

  static void test_arena_consolidation()
  {
    reset_backing();
    TestArena arena;

    auto a = unit_addr(0);
    auto b = unit_addr(4);
    arena.add_block(a, unit_size(4));
    arena.check_invariant(true);
    auto [ov_a, ov_s] = arena.add_block(b, unit_size(4));
    SNMALLOC_CHECK(ov_a == 0 && ov_s == 0);
    arena.check_invariant(true);

    // A single 8-unit block should now be removable from the
    // consolidated region.
    auto got = arena.remove_block(unit_size(8));
    SNMALLOC_CHECK(got == a);
    arena.check_invariant(true);

    printf("  Arena consolidation: OK\n");
  }

  // ==================================================================
  // (E) Carving: request a smaller size than the free block has.
  // ==================================================================

  static void test_arena_carve()
  {
    reset_backing();
    TestArena arena;

    auto a = unit_addr(0);
    arena.add_block(a, unit_size(8));
    arena.check_invariant(true);

    auto got = arena.remove_block(unit_size(3));
    SNMALLOC_CHECK(got != 0);
    arena.check_invariant(true);

    // The remainder is still available; total removed should sum to
    // 8 units across this and subsequent removes.
    size_t total_removed = 3;
    while (true)
    {
      auto r = arena.remove_block(unit_size(1));
      if (r == 0)
        break;
      total_removed += 1;
      arena.check_invariant(true);
    }
    SNMALLOC_CHECK(total_removed == 8);

    printf("  Arena carve + drain: OK\n");
  }

  // ==================================================================
  // (F) Randomised stress: oracle-checked add/remove over a single
  // chunk's worth of units. Equivalent to the MockRep stress test in
  // shape but operates on real in-band storage.
  // ==================================================================

  static constexpr size_t STRESS_UNITS =
    (size_t(1) << MAX_SIZE_BITS) / UNIT_SIZE - 1;

  using Bins = ArenaBins<2, MIN_BITS>;

  struct OracleRange
  {
    size_t addr_units;
    size_t size_units;

    bool operator<(const OracleRange& o) const
    {
      return addr_units < o.addr_units;
    }
  };

  // Mirrors the arena's bin-based allocator: classify entries into
  // bins, pick the bin via the bitmap's find_for_request, then
  // pick the lowest-address entry within that bin and carve.
  class Oracle
  {
    std::set<OracleRange> ranges;

  public:
    void add(size_t addr_units, size_t size_units)
    {
      OracleRange key{addr_units, size_units};
      auto it = ranges.lower_bound(key);

      size_t new_addr = addr_units;
      size_t new_size = size_units;

      if (it != ranges.end() && it->addr_units == new_addr + new_size)
      {
        new_size += it->size_units;
        it = ranges.erase(it);
      }

      if (it != ranges.begin())
      {
        auto prev = std::prev(it);
        if (prev->addr_units + prev->size_units == new_addr)
        {
          new_addr = prev->addr_units;
          new_size += prev->size_units;
          ranges.erase(prev);
        }
      }

      ranges.insert({new_addr, new_size});
    }

    // Returns {addr_units, len_units} or {0, 0} if nothing fits.
    std::pair<size_t, size_t> remove(size_t n_units)
    {
      size_t n_bytes = n_units * UNIT_SIZE;
      if (n_bytes == 0 || n_bytes > Bins::max_supported_size())
        return {0, 0};

      typename Bins::Bitmap bm{};
      std::map<size_t, std::vector<std::set<OracleRange>::iterator>> by_bin;

      for (auto it = ranges.begin(); it != ranges.end(); ++it)
      {
        typename Bins::range_t r{
          unit_addr(it->addr_units), it->size_units * UNIT_SIZE};
        size_t bin = bm.add(r);
        by_bin[bin].push_back(it);
      }

      size_t bin_id = bm.find_for_request(n_bytes);
      if (bin_id == SIZE_MAX)
        return {0, 0};

      auto& entries = by_bin[bin_id];
      auto best_it = entries[0];
      for (size_t i = 1; i < entries.size(); i++)
      {
        if (entries[i]->addr_units < best_it->addr_units)
          best_it = entries[i];
      }

      OracleRange block = *best_it;
      ranges.erase(best_it);

      auto carved = Bins::carve(
        {unit_addr(block.addr_units), block.size_units * UNIT_SIZE}, n_bytes);
      auto base = base_addr();
      if (carved.pre.size != 0)
        ranges.insert(
          {(carved.pre.base - base) / UNIT_SIZE, carved.pre.size / UNIT_SIZE});
      if (carved.post.size != 0)
        ranges.insert(
          {(carved.post.base - base) / UNIT_SIZE,
           carved.post.size / UNIT_SIZE});

      return {
        (carved.req.base - base) / UNIT_SIZE, carved.req.size / UNIT_SIZE};
    }
  };

  static void test_stress_seed(size_t seed, size_t num_ops)
  {
    reset_backing();
    TestArena arena;
    Oracle oracle;

    // All units initially allocated (i.e., not in the arena).
    std::vector<bool> allocated(STRESS_UNITS, true);

    xoroshiro::p128r64 rng(seed);

    for (size_t op = 0; op < num_ops; op++)
    {
      bool do_add = (rng.next() % 3) != 0;

      if (do_add)
      {
        size_t max_size = STRESS_UNITS / 4;
        if (max_size < 1)
          max_size = 1;
        size_t size = (rng.next() % max_size) + 1;
        size_t start = rng.next() % STRESS_UNITS;

        bool found = false;
        for (size_t try_start = start; try_start < STRESS_UNITS; try_start++)
        {
          size_t actual = 0;
          for (size_t j = try_start; j < STRESS_UNITS && j < try_start + size;
               j++)
          {
            if (!allocated[j])
              break;
            actual++;
          }
          if (actual >= 1)
          {
            size = actual;
            start = try_start;
            found = true;
            break;
          }
        }
        if (!found)
          continue;

        for (size_t j = start; j < start + size; j++)
          allocated[j] = false;

        auto result = arena.add_block(unit_addr(start), unit_size(size));
        if (result.first == 0)
          oracle.add(start, size);
        else
        {
          // Overflow: arena spilled the consolidated block back to
          // the caller. Treat as if everything went back to
          // "allocated"; clear the oracle.
          for (size_t j = 0; j < STRESS_UNITS; j++)
            allocated[j] = true;
          oracle = Oracle{};
        }
        arena.check_invariant(true);
      }
      else
      {
        size_t max_req = STRESS_UNITS / 4;
        if (max_req < 1)
          max_req = 1;
        size_t n = (rng.next() % max_req) + 1;

        auto arena_addr = arena.remove_block(unit_size(n));
        auto [o_start, o_len] = oracle.remove(n);

        if (o_len == 0)
        {
          SNMALLOC_CHECK(arena_addr == 0);
        }
        else
        {
          SNMALLOC_CHECK(arena_addr != 0);
          SNMALLOC_CHECK(arena_addr == unit_addr(o_start));
          for (size_t j = o_start; j < o_start + o_len; j++)
            allocated[j] = true;
        }
        arena.check_invariant(true);
      }
    }
  }

  static void test_stress()
  {
    constexpr size_t NUM_OPS = 500;
    constexpr size_t NUM_SEEDS = 30;
    for (size_t s = 1; s <= NUM_SEEDS; s++)
      test_stress_seed(s, NUM_OPS);
    printf("  Stress (%zu seeds x %zu ops): OK\n", NUM_SEEDS, NUM_OPS);
  }

  // ==================================================================
  // (G) SmallArenaRange — chunk-granularity parent + sub-chunk
  // sub-allocations served by the in-band arena.
  // ==================================================================

  // Pool of chunk-aligned buffers, handed out as a chunk-granularity
  // parent range to SmallArenaRange. Oversized by MIN_CHUNK_SIZE so
  // `pool_base()` can align up at runtime — MSVC rejects alignas
  // values as large as MIN_CHUNK_SIZE on static storage.
  static constexpr size_t POOL_CHUNKS = 8;
  static unsigned char pool_storage[(POOL_CHUNKS + 1) * MIN_CHUNK_SIZE];
  static bool pool_in_use[POOL_CHUNKS];
  // Track returns to detect leaks / double-frees.
  static size_t pool_alloc_count;
  static size_t pool_dealloc_count;

  static unsigned char* pool_base()
  {
    uintptr_t raw = reinterpret_cast<uintptr_t>(&pool_storage[0]);
    uintptr_t aligned = (raw + MIN_CHUNK_SIZE - 1) & ~(MIN_CHUNK_SIZE - 1);
    return reinterpret_cast<unsigned char*>(aligned);
  }

  static void reset_pool()
  {
    for (size_t i = 0; i < POOL_CHUNKS; i++)
      pool_in_use[i] = false;
    for (size_t i = 0; i < sizeof(pool_storage); i++)
      pool_storage[i] = 0;
    pool_alloc_count = 0;
    pool_dealloc_count = 0;
  }

  class MockParent
  {
  public:
    static constexpr bool Aligned = true;
    static constexpr bool ConcurrencySafe = true;
    using ChunkBounds = capptr::bounds::Arena;

    constexpr MockParent() = default;

    CapPtr<void, ChunkBounds> alloc_range(size_t size)
    {
      SNMALLOC_CHECK(size == MIN_CHUNK_SIZE);
      for (size_t i = 0; i < POOL_CHUNKS; i++)
      {
        if (!pool_in_use[i])
        {
          pool_in_use[i] = true;
          pool_alloc_count++;
          return CapPtr<void, ChunkBounds>::unsafe_from(
            pool_base() + i * MIN_CHUNK_SIZE);
        }
      }
      return nullptr;
    }

    void dealloc_range(CapPtr<void, ChunkBounds> base, size_t size)
    {
      SNMALLOC_CHECK(size == MIN_CHUNK_SIZE);
      auto p = static_cast<unsigned char*>(base.unsafe_ptr());
      auto idx = static_cast<size_t>(p - pool_base()) / MIN_CHUNK_SIZE;
      SNMALLOC_CHECK(idx < POOL_CHUNKS);
      SNMALLOC_CHECK(pool_in_use[idx]);
      pool_in_use[idx] = false;
      pool_dealloc_count++;
    }
  };

  using SmallArena = SmallArenaRange<DummyAuthmap>::Type<MockParent>;

  static void test_small_arena_basic()
  {
    reset_pool();
    SmallArena r;

    // First alloc triggers a refill of one chunk; the rest of the
    // chunk is internally available for further sub-allocations.
    auto a = r.alloc_range(UNIT_SIZE);
    SNMALLOC_CHECK(a != nullptr);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    auto b = r.alloc_range(unit_size(3));
    SNMALLOC_CHECK(b != nullptr);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    // Non-pow2 size — the whole point of SmallArenaRange.
    auto c = r.alloc_range(unit_size(5));
    SNMALLOC_CHECK(c != nullptr);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    r.dealloc_range(a, UNIT_SIZE);
    r.dealloc_range(b, unit_size(3));
    r.dealloc_range(c, unit_size(5));

    printf("  SmallArenaRange basic alloc/dealloc: OK\n");
  }

  static void test_small_arena_chunk_pass_through()
  {
    reset_pool();
    SmallArena r;

    // A chunk-or-larger alloc should pass through to the parent
    // without touching the arena.
    auto a = r.alloc_range(MIN_CHUNK_SIZE);
    SNMALLOC_CHECK(a != nullptr);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    r.dealloc_range(a, MIN_CHUNK_SIZE);
    SNMALLOC_CHECK(pool_dealloc_count == 1);

    printf("  SmallArenaRange chunk pass-through: OK\n");
  }

  static void test_small_arena_unaligned_dealloc()
  {
    reset_pool();
    SmallArena r;

    // Get some sub-chunk space populated.
    auto a = r.alloc_range(unit_size(4));
    SNMALLOC_CHECK(a != nullptr);

    // Donate an unaligned spare (mirrors make()'s spare-seed
    // donation). Length is not unit-aligned; sub-unit edges must
    // be silently discarded.
    auto unaligned_base = pointer_offset(a, 1);
    r.dealloc_range(unaligned_base, unit_size(4) - 1);

    // Should not have leaked chunks to the parent (sub-chunk
    // fragments stay in the arena).
    SNMALLOC_CHECK(pool_dealloc_count == 0);

    printf("  SmallArenaRange unaligned dealloc: OK\n");
  }

  static void test_small_arena_consolidation_returns_chunk()
  {
    reset_pool();
    SmallArena r;

    // Fully consume one chunk via small allocs; record the chunk
    // base so we can rebuild the full chunk via deallocs.
    constexpr size_t N = MIN_CHUNK_SIZE / UNIT_SIZE;
    std::vector<CapPtr<void, capptr::bounds::Arena>> ps;
    for (size_t i = 0; i < N; i++)
    {
      auto p = r.alloc_range(UNIT_SIZE);
      SNMALLOC_CHECK(p != nullptr);
      ps.push_back(p);
    }
    // We expect at least one refill happened (likely just one,
    // since N units == one chunk; but in either case all
    // sub-allocs come from the same backing chunk).
    SNMALLOC_CHECK(pool_alloc_count >= 1);

    size_t deallocs_before = pool_dealloc_count;
    for (auto p : ps)
      r.dealloc_range(p, UNIT_SIZE);

    // Consolidation should reassemble the whole chunk and donate
    // it back to the parent.
    SNMALLOC_CHECK(pool_dealloc_count > deallocs_before);

    printf("  SmallArenaRange consolidation returns chunk: OK\n");
  }

  // alloc_size_with_align

  static void test_alloc_size_with_align_exact()
  {
    reset_pool();
    SmallArena r;

    size_t size = unit_size(4);
    size_t align = UNIT_SIZE;
    auto p = r.alloc_size_with_align(size, align);
    SNMALLOC_CHECK(p != nullptr);
    SNMALLOC_CHECK((address_cast(p) & (align - 1)) == 0);

    r.dealloc_range(p, size);
    printf("  alloc_size_with_align exact (no leftover): OK\n");
  }

  static void test_alloc_size_with_align_pow2_align_over_size()
  {
    reset_pool();
    SmallArena r;

    size_t size = unit_size(3) + 2;
    size_t align = 256;
    SNMALLOC_CHECK(align <= MIN_CHUNK_SIZE);
    SNMALLOC_CHECK(align >= UNIT_SIZE);
    SNMALLOC_CHECK(bits::is_pow2(align));

    auto p = r.alloc_size_with_align(size, align);
    SNMALLOC_CHECK(p != nullptr);
    SNMALLOC_CHECK((address_cast(p) & (align - 1)) == 0);

    size_t used = bits::align_up(size, UNIT_SIZE);
    size_t requested = bits::align_up(size, align);
    SNMALLOC_CHECK(requested - used > 0);

    // Donated tail and the carved-but-unused chunk remainder both
    // sit in the arena, so the follow-up alloc must succeed
    // without a second parent refill — exact address is not
    // pinned down.
    auto tail = r.alloc_range(requested - used);
    SNMALLOC_CHECK(tail != nullptr);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    r.dealloc_range(p, used);
    r.dealloc_range(tail, requested - used);
    printf("  alloc_size_with_align pow2 align over non-pow2 size: OK\n");
  }

  static void test_alloc_size_with_align_align_larger_than_size()
  {
    reset_pool();
    SmallArena r;

    // User's motivating example, scaled into the test arena.
    size_t align = 4096;
    SNMALLOC_CHECK(align <= MIN_CHUNK_SIZE);
    size_t size = align - 254;

    auto p = r.alloc_size_with_align(size, align);
    SNMALLOC_CHECK(p != nullptr);
    SNMALLOC_CHECK((address_cast(p) & (align - 1)) == 0);

    size_t used = bits::align_up(size, UNIT_SIZE);
    auto tail = r.alloc_range(align - used);
    SNMALLOC_CHECK(tail != nullptr);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    r.dealloc_range(p, used);
    r.dealloc_range(tail, align - used);
    printf("  alloc_size_with_align align > size: OK\n");
  }

  static void test_alloc_size_with_align_chunk_bypass()
  {
    reset_pool();
    SmallArena r;

    size_t size = MIN_CHUNK_SIZE - 100;
    size_t align = MIN_CHUNK_SIZE;
    auto p = r.alloc_size_with_align(size, align);
    SNMALLOC_CHECK(p != nullptr);
    SNMALLOC_CHECK((address_cast(p) & (align - 1)) == 0);
    SNMALLOC_CHECK(pool_alloc_count == 1);

    // requested == MIN_CHUNK_SIZE bypasses to parent (whole chunk,
    // no carve-time leftover), so the only free arena fragment is
    // the donated tail — pin its exact address. Tail stays
    // intra-chunk, so no dealloc to parent.
    SNMALLOC_CHECK(pool_dealloc_count == 0);

    size_t used = bits::align_up(size, UNIT_SIZE);
    if (used < MIN_CHUNK_SIZE)
    {
      auto tail = r.alloc_range(MIN_CHUNK_SIZE - used);
      SNMALLOC_CHECK(tail != nullptr);
      SNMALLOC_CHECK(address_cast(tail) == address_cast(p) + used);
      r.dealloc_range(tail, MIN_CHUNK_SIZE - used);
    }
    r.dealloc_range(p, used);

    printf("  alloc_size_with_align chunk-sized bypass: OK\n");
  }
} // namespace snmalloc

int main()
{
  printf("--- InplaceRep tests ---\n");
  printf(
    "  UNIT_SIZE=%zu, MIN_BITS=%zu, MAX_SIZE_BITS=%zu, STRESS_UNITS=%zu\n",
    snmalloc::UNIT_SIZE,
    snmalloc::MIN_BITS,
    snmalloc::MAX_SIZE_BITS,
    snmalloc::STRESS_UNITS);

  printf("(A) Accessor round-trips:\n");
  snmalloc::test_variant_roundtrip();
  snmalloc::test_large_size_roundtrip();

  printf("(B) Red bits independent:\n");
  snmalloc::test_red_bits_independent();
  snmalloc::test_can_consolidate_chunk_boundary();

  printf("(C) Arena add/remove:\n");
  snmalloc::test_arena_add_remove_single();

  printf("(D) Arena consolidation:\n");
  snmalloc::test_arena_consolidation();

  printf("(E) Arena carve:\n");
  snmalloc::test_arena_carve();

  printf("(F) Stress:\n");
  snmalloc::test_stress();

  printf("(G) SmallArenaRange:\n");
  snmalloc::test_small_arena_basic();
  snmalloc::test_small_arena_chunk_pass_through();
  snmalloc::test_small_arena_unaligned_dealloc();
  snmalloc::test_small_arena_consolidation_returns_chunk();
  snmalloc::test_alloc_size_with_align_exact();
  snmalloc::test_alloc_size_with_align_pow2_align_over_size();
  snmalloc::test_alloc_size_with_align_align_larger_than_size();
  snmalloc::test_alloc_size_with_align_chunk_bypass();

  printf("All InplaceRep tests passed.\n");
  return 0;
}
