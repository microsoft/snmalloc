/**
 * Unit tests for BitmapCoalesce — core insert/remove/coalescing.
 *
 * Uses a mock Rep (in-memory array) to simulate pagemap entries.
 * Tests the bitmap + free-list machinery in isolation.
 */

#include <cstring>
#include <iostream>
#include <vector>

#include <snmalloc/snmalloc_core.h>
#include <test/setup.h>

using namespace snmalloc;

static constexpr size_t TEST_MAX_SIZE_BITS = bits::BITS - 1;
using BC = BitmapCoalesceHelpers<TEST_MAX_SIZE_BITS>;

// ---- Mock Rep ----

static constexpr size_t ARENA_CHUNKS = 256;
static constexpr address_t ARENA_BASE = MIN_CHUNK_SIZE; // chunk index 1
static constexpr size_t ARENA_SIZE = ARENA_CHUNKS * MIN_CHUNK_SIZE;

struct MockEntry
{
  address_t next_ptr = 0;
  size_t block_size = 0;
  bool coalesce_free = false;
  bool boundary = false;
};

static MockEntry mock_entries[ARENA_CHUNKS];

struct MockRep
{
  static size_t idx(address_t addr)
  {
    SNMALLOC_ASSERT(addr >= ARENA_BASE);
    SNMALLOC_ASSERT(addr < ARENA_BASE + ARENA_SIZE);
    SNMALLOC_ASSERT(addr % MIN_CHUNK_SIZE == 0);
    return (addr - ARENA_BASE) / MIN_CHUNK_SIZE;
  }

  static address_t get_next(address_t addr)
  {
    return mock_entries[idx(addr)].next_ptr;
  }

  static void set_next(address_t addr, address_t next)
  {
    mock_entries[idx(addr)].next_ptr = next;
  }

  static size_t get_size(address_t addr)
  {
    return mock_entries[idx(addr)].block_size;
  }

  static void set_size(address_t addr, size_t size)
  {
    mock_entries[idx(addr)].block_size = size;
  }

  static void set_boundary_tags(address_t addr, size_t size)
  {
    set_size(addr, size);
    if (size > MIN_CHUNK_SIZE)
      set_size(addr + size - MIN_CHUNK_SIZE, size);
  }

  static bool is_free_block(address_t addr)
  {
    if (addr < ARENA_BASE || addr >= ARENA_BASE + ARENA_SIZE)
      return false;
    if (addr % MIN_CHUNK_SIZE != 0)
      return false;
    return mock_entries[idx(addr)].coalesce_free;
  }

  static bool is_boundary(address_t addr)
  {
    if (addr < ARENA_BASE || addr >= ARENA_BASE + ARENA_SIZE)
      return true;
    if (addr % MIN_CHUNK_SIZE != 0)
      return true;
    return mock_entries[idx(addr)].boundary;
  }

  static void set_boundary(address_t addr)
  {
    mock_entries[idx(addr)].boundary = true;
  }

  static void set_coalesce_free(address_t addr)
  {
    mock_entries[idx(addr)].coalesce_free = true;
  }

  static void clear_coalesce_free(address_t addr)
  {
    mock_entries[idx(addr)].coalesce_free = false;
  }
};

using BCCore = BitmapCoalesce<MockRep, TEST_MAX_SIZE_BITS>;

static void reset_mock()
{
  for (auto& e : mock_entries)
    e = MockEntry{};
}

// ---- Test framework ----

static size_t failure_count = 0;

static void check(bool cond, const char* msg, size_t line)
{
  if (!cond)
  {
    std::cout << "FAIL (line " << line << "): " << msg << std::endl;
    failure_count++;
  }
}

#define CHECK(cond) check(cond, #cond, __LINE__)

// Helper: convert chunk index (relative to arena) to byte address.
static address_t chunk_addr(size_t chunk_idx)
{
  return ARENA_BASE + chunk_idx * MIN_CHUNK_SIZE;
}

// Helper: convert chunk count to byte size.
static size_t chunk_size(size_t n)
{
  return n * MIN_CHUNK_SIZE;
}

// ---- Test: insert/remove round-trip ----

void test_insert_remove_roundtrip()
{
  std::cout << "test_insert_remove_roundtrip..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a 4-chunk block at address with alignment 4 (chunk 4).
  // chunk_addr(3) = ARENA_BASE + 3*MCS = 4*MCS, so chunk index in
  // address space is 4, alignment = 4.
  address_t addr = chunk_addr(3); // byte addr = 4 * MCS
  size_t sz = chunk_size(4);
  bc.add_fresh_range(addr, sz);

  // The block can serve sizeclass 4 (e=2, m=0) since alpha=4 and n=4.
  // T(4, 4) = 4.
  auto result = bc.remove_block(chunk_size(4));
  CHECK(result.addr == addr);
  CHECK(result.size == sz);

  // After removal, another remove should return empty.
  auto empty = bc.remove_block(chunk_size(4));
  CHECK(empty.addr == 0);
  CHECK(empty.size == 0);
}

// ---- Test: bitmap correctness ----

void test_bitmap_correctness()
{
  std::cout << "test_bitmap_correctness..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a block.
  address_t addr = chunk_addr(3); // 4*MCS, alpha=4
  bc.add_fresh_range(addr, chunk_size(4));

  // Compute expected bin.
  size_t bin = BC::bin_index(4, 4);
  CHECK(bc.is_bin_non_empty(bin));
  CHECK(bc.get_bin_head(bin) == addr);

  // Remove it.
  bc.remove_block(chunk_size(4));

  // Bitmap bit should be cleared.
  CHECK(!bc.is_bin_non_empty(bin));
  CHECK(bc.get_bin_head(bin) == 0);
}

// ---- Test: multiple bins ----

void test_multiple_bins()
{
  std::cout << "test_multiple_bins..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Block A: 4 chunks at chunk 4 (alpha=4) → bin_index(4, 4)
  address_t a_addr = chunk_addr(3); // 4*MCS
  bc.add_fresh_range(a_addr, chunk_size(4));

  // Block B: 8 chunks at chunk 8 (alpha=8) → bin_index(8, 8)
  address_t b_addr = chunk_addr(7); // 8*MCS
  bc.add_fresh_range(b_addr, chunk_size(8));

  size_t bin_a = BC::bin_index(4, 4);
  size_t bin_b = BC::bin_index(8, 8);
  CHECK(bin_a != bin_b);
  CHECK(bc.is_bin_non_empty(bin_a));
  CHECK(bc.is_bin_non_empty(bin_b));

  // Remove from bin A (sizeclass 4).
  auto result = bc.remove_block(chunk_size(4));
  CHECK(result.addr == a_addr);

  // Block B should still be there.
  CHECK(bc.is_bin_non_empty(bin_b));
  CHECK(!bc.is_bin_non_empty(bin_a));
}

// ---- Test: best-fit search ----

void test_best_fit_search()
{
  std::cout << "test_best_fit_search..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a small block that can't serve sizeclass 8.
  // Block at chunk 4, size 4, alpha=4 → can serve sc 4 but not 8.
  bc.add_fresh_range(chunk_addr(3), chunk_size(4));

  // Insert a larger block that can serve sizeclass 8.
  // Block at chunk 16, size 8, alpha=16. bin_index(8, 16).
  // T(8, 16) = 8. So 8 >= 8 → A-only at e=3.
  bc.add_fresh_range(chunk_addr(15), chunk_size(8));

  // Allocate sizeclass 8.  The small block can't serve it; the large
  // block should be returned.
  auto result = bc.remove_block(chunk_size(8));
  CHECK(result.addr == chunk_addr(15));
  CHECK(result.size == chunk_size(8));
}

// ---- Test: masked search for m=0 ----

void test_masked_search()
{
  std::cout << "test_masked_search..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a block that is B-only at e=2: serves m=1 (sc 5) but not
  // m=0 (sc 4).  Need n >= T(5, alpha) and n < T(4, alpha).
  //
  // At alpha=1: T(5,1) = 5, T(4,1) = 7.  So n=5 at alpha=1 works:
  // bin_index(5, 1) should give B-only at e=2.
  //
  // Use chunk 1 (addr = ARENA_BASE = MCS, chunk_addr in addr space = 1,
  // alpha = 1).
  address_t bonly_addr = chunk_addr(0); // addr = 1*MCS, alpha = 1
  bc.add_fresh_range(bonly_addr, chunk_size(5));

  size_t bin = BC::bin_index(5, 1);
  size_t expected_bonly = BC::exponent_base_bit(2) + BC::SLOT_B_ONLY;
  CHECK(bin == expected_bonly);

  // Allocate m=0 at e=2 (sizeclass 4).  The B-only block should be
  // skipped by the mask.
  auto result = bc.remove_block(chunk_size(4));
  CHECK(result.addr == 0); // Not found — B-only is masked.
  CHECK(result.size == 0);

  // Allocate m=1 at e=2 (sizeclass 5).  The B-only block should be found.
  result = bc.remove_block(chunk_size(5));
  CHECK(result.addr == bonly_addr);
  CHECK(result.size == chunk_size(5));
}

// ---- Test: remove_from_bin correctness ----

void test_remove_from_bin()
{
  std::cout << "test_remove_from_bin..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert 3 blocks into the same bin.  We need blocks at different
  // addresses but same (n_chunks, alpha).
  //
  // Use blocks of 4 chunks at addresses with alpha=4:
  //   chunk 4  (addr = 4*MCS),  alpha = 4
  //   chunk 12 (addr = 12*MCS), alpha = 4 (12 = 4*3, natural_alignment = 4)
  //   chunk 20 (addr = 20*MCS), alpha = 4 (20 = 4*5, natural_alignment = 4)
  address_t a1 = chunk_addr(3);  // 4*MCS
  address_t a2 = chunk_addr(11); // 12*MCS
  address_t a3 = chunk_addr(19); // 20*MCS

  bc.add_fresh_range(a1, chunk_size(4));
  bc.add_fresh_range(a2, chunk_size(4));
  bc.add_fresh_range(a3, chunk_size(4));

  size_t bin = BC::bin_index(4, 4);
  CHECK(bc.is_bin_non_empty(bin));

  // The list should be: a3 -> a2 -> a1 -> 0 (prepend order).
  CHECK(bc.get_bin_head(bin) == a3);

  // Remove the middle block (a2) using remove_block(addr, size).
  // We need to use add_block for this test since remove_from_bin is private.
  // Actually, we test it indirectly through add_block's coalescing.
  // For now, let's verify that remove_block (allocation) pops the head.

  // Remove: should get a3 (head).
  auto r1 = bc.remove_block(chunk_size(4));
  CHECK(r1.addr == a3);

  // Next remove: should get a2.
  auto r2 = bc.remove_block(chunk_size(4));
  CHECK(r2.addr == a2);

  // Next remove: should get a1.
  auto r3 = bc.remove_block(chunk_size(4));
  CHECK(r3.addr == a1);

  // Now empty.
  auto r4 = bc.remove_block(chunk_size(4));
  CHECK(r4.addr == 0);
}

// ---- Test: empty return ----

void test_empty_return()
{
  std::cout << "test_empty_return..." << std::endl;
  reset_mock();
  BCCore bc{};

  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == 0);
  CHECK(result.size == 0);

  result = bc.remove_block(chunk_size(4));
  CHECK(result.addr == 0);
  CHECK(result.size == 0);
}

// ---- Test: boundary tags ----

void test_boundary_tags()
{
  std::cout << "test_boundary_tags..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a 4-chunk block and verify boundary tags are set.
  address_t addr = chunk_addr(3); // 4*MCS
  bc.add_fresh_range(addr, chunk_size(4));

  // First chunk should have size = 4*MCS.
  CHECK(MockRep::get_size(addr) == chunk_size(4));
  // Last chunk should also have size = 4*MCS.
  CHECK(MockRep::get_size(addr + chunk_size(3)) == chunk_size(4));
  // coalesce_free should be set on first and last.
  CHECK(MockRep::is_free_block(addr));
  CHECK(MockRep::is_free_block(addr + chunk_size(3)));

  // After removal, first-entry tags should be cleared.
  bc.remove_block(chunk_size(4));
  CHECK(MockRep::get_size(addr) == 0);
  CHECK(!MockRep::is_free_block(addr));
  // Last-entry tag is intentionally left stale.
  CHECK(MockRep::get_size(addr + chunk_size(3)) == chunk_size(4));
}

// ---- Test: single-chunk block ----

void test_single_chunk()
{
  std::cout << "test_single_chunk..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Single-chunk block at chunk 2 (alpha=2).
  address_t addr = chunk_addr(1); // 2*MCS
  bc.add_fresh_range(addr, chunk_size(1));

  // Can serve sizeclass 1.
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == addr);
  CHECK(result.size == chunk_size(1));
}

// ---- Test: large block serves small sizeclass ----

void test_large_serves_small()
{
  std::cout << "test_large_serves_small..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a large block (16 chunks) at a well-aligned address.
  // chunk 16 (alpha=16).
  address_t addr = chunk_addr(15); // 16*MCS
  bc.add_fresh_range(addr, chunk_size(16));

  // Should be able to serve sizeclass 1 (the search walks up from bit 0).
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == addr);
  CHECK(result.size == chunk_size(16));
}

// ---- Test: coalesce right ----

void test_coalesce_right()
{
  std::cout << "test_coalesce_right..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert block A.
  address_t a_addr = chunk_addr(3); // 4*MCS
  bc.add_fresh_range(a_addr, chunk_size(4));

  // add_block B immediately to A's right → should coalesce.
  address_t b_addr = a_addr + chunk_size(4);
  bc.add_block(b_addr, chunk_size(4));

  // Should now have a single 8-chunk block at a_addr.
  // The combined block's bin depends on (8 chunks, alpha(a_addr)).
  // alpha(4*MCS) = alpha(chunk 4) = 4.  bin_index(8, 4).
  size_t combined_bin = BC::bin_index(8, 4);
  CHECK(bc.is_bin_non_empty(combined_bin));
  CHECK(bc.get_bin_head(combined_bin) == a_addr);

  // Remove it and verify size.
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == a_addr);
  CHECK(result.size == chunk_size(8));
}

// ---- Test: coalesce left ----

void test_coalesce_left()
{
  std::cout << "test_coalesce_left..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert block B (the right one first).
  address_t b_addr = chunk_addr(7); // 8*MCS
  bc.add_fresh_range(b_addr, chunk_size(4));

  // add_block A immediately to B's left → should coalesce.
  address_t a_addr = chunk_addr(3); // 4*MCS
  bc.add_block(a_addr, chunk_size(4));

  // Should have a single 8-chunk block at a_addr.
  size_t combined_bin =
    BC::bin_index(8, BC::natural_alignment(a_addr / MIN_CHUNK_SIZE));
  CHECK(bc.is_bin_non_empty(combined_bin));
  CHECK(bc.get_bin_head(combined_bin) == a_addr);
}

// ---- Test: coalesce both sides ----

void test_coalesce_both()
{
  std::cout << "test_coalesce_both..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert blocks A and C with a gap between.
  address_t a_addr = chunk_addr(3);  // 4*MCS, 4 chunks
  address_t c_addr = chunk_addr(11); // 12*MCS, 4 chunks

  bc.add_fresh_range(a_addr, chunk_size(4));
  bc.add_fresh_range(c_addr, chunk_size(4));

  // add_block the gap (chunks 8-11 = 4 chunks from 8*MCS).
  address_t gap_addr = chunk_addr(7); // 8*MCS
  bc.add_block(gap_addr, chunk_size(4));

  // Should have a single 12-chunk block at a_addr.
  size_t combined_bin =
    BC::bin_index(12, BC::natural_alignment(a_addr / MIN_CHUNK_SIZE));
  CHECK(bc.is_bin_non_empty(combined_bin));
  CHECK(bc.get_bin_head(combined_bin) == a_addr);

  // Verify by removing.
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == a_addr);
  CHECK(result.size == chunk_size(12));
}

// ---- Test: boundary prevents coalescing ----

void test_boundary_prevents_coalescing()
{
  std::cout << "test_boundary_prevents_coalescing..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert blocks A and B adjacent.
  address_t a_addr = chunk_addr(3); // 4*MCS, 4 chunks
  address_t b_addr = chunk_addr(7); // 8*MCS, 4 chunks

  bc.add_fresh_range(a_addr, chunk_size(4));
  bc.add_fresh_range(b_addr, chunk_size(4));

  // Set a boundary between A and B (at B's start).
  MockRep::set_boundary(b_addr);

  // add_block C adjacent to B's right → should merge with B, not A.
  address_t c_addr = chunk_addr(11); // 12*MCS
  bc.add_block(c_addr, chunk_size(4));

  // B+C should be coalesced into 8 chunks at b_addr.
  size_t bc_bin =
    BC::bin_index(8, BC::natural_alignment(b_addr / MIN_CHUNK_SIZE));
  CHECK(bc.is_bin_non_empty(bc_bin));

  // A should still be in its original bin.
  size_t a_bin =
    BC::bin_index(4, BC::natural_alignment(a_addr / MIN_CHUNK_SIZE));
  CHECK(bc.is_bin_non_empty(a_bin));
}

// ---- Test: stale tag after remove ----

void test_stale_tag_after_remove()
{
  std::cout << "test_stale_tag_after_remove..." << std::endl;
  reset_mock();
  BCCore bc{};

  // add_fresh_range block A.
  address_t a_addr = chunk_addr(3); // 4*MCS, 4 chunks
  bc.add_fresh_range(a_addr, chunk_size(4));

  // Allocate A (this clears first-entry coalesce_free and size).
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == a_addr);

  // Now add_block B immediately to A's right.  The left walk should see
  // that A's first entry is not coalesce_free and stop — no coalescing.
  address_t b_addr = chunk_addr(7); // 8*MCS, 4 chunks
  bc.add_block(b_addr, chunk_size(4));

  // B should be in its own bin, not merged with ghost of A.
  size_t b_bin =
    BC::bin_index(4, BC::natural_alignment(b_addr / MIN_CHUNK_SIZE));
  CHECK(bc.is_bin_non_empty(b_bin));
  CHECK(bc.get_bin_head(b_bin) == b_addr);

  // Remove B and verify it's still 4 chunks (not 8).
  result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == b_addr);
  CHECK(result.size == chunk_size(4));
}

// ---- Test: stale tag after right-walk absorption ----

void test_stale_tag_after_absorption()
{
  std::cout << "test_stale_tag_after_absorption..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert A, B, C adjacent: chunks 4-7, 8-11, 12-15.
  address_t a_addr = chunk_addr(3);  // 4*MCS
  address_t b_addr = chunk_addr(7);  // 8*MCS
  address_t c_addr = chunk_addr(11); // 12*MCS
  bc.add_fresh_range(a_addr, chunk_size(4));
  bc.add_fresh_range(b_addr, chunk_size(4));
  bc.add_fresh_range(c_addr, chunk_size(4));

  // add_block a block to the left of A.  The right walk absorbs A, B, C.
  address_t left_addr = chunk_addr(1); // 2*MCS, 2 chunks
  bc.add_block(left_addr, chunk_size(2));

  // Should have one big block: 2+4+4+4 = 14 chunks at left_addr.
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == left_addr);
  CHECK(result.size == chunk_size(14));

  // Now the stale tags from B and C should have been cleared.
  // add_block a block to the right of the merged region.
  // The merged region ends at chunk_addr(1) + 14*MCS = chunk_addr(15).
  address_t right_addr = chunk_addr(15); // 16*MCS
  bc.add_block(right_addr, chunk_size(4));

  // Should NOT read stale tags from B or C and should stay as 4 chunks.
  result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == right_addr);
  CHECK(result.size == chunk_size(4));
}

// ---- Test: underflow guard (address 0) ----
// Can't directly test address 0 with our arena base at MCS.
// But we can test that the left walk stops at the arena boundary.

void test_left_walk_stops_at_boundary()
{
  std::cout << "test_left_walk_stops_at_boundary..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert block at the very start of the arena.
  address_t addr = chunk_addr(0); // ARENA_BASE = 1*MCS
  bc.add_fresh_range(addr, chunk_size(4));

  // add_block another block adjacent to its right.
  address_t right_addr = chunk_addr(4); // 5*MCS
  bc.add_block(right_addr, chunk_size(4));

  // Should coalesce into one 8-chunk block at chunk_addr(0).
  auto result = bc.remove_block(chunk_size(1));
  CHECK(result.addr == addr);
  CHECK(result.size == chunk_size(8));

  // Now insert a block at chunk_addr(0) again.
  bc.add_fresh_range(addr, chunk_size(4));

  // add_block at its left should be impossible (out of arena).
  // But we can verify the block stays at 4 chunks.
  auto result2 = bc.remove_block(chunk_size(1));
  CHECK(result2.addr == addr);
  CHECK(result2.size == chunk_size(4));
}

// ---- Test: stress (random alloc/free) ----

void test_stress()
{
  std::cout << "test_stress..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Use a deterministic PRNG for reproducibility.
  uint32_t seed = 12345;
  auto rng = [&seed]() -> uint32_t {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };

  // Track live allocations (addr -> size).
  struct Alloc
  {
    address_t addr;
    size_t size;
  };
  std::vector<Alloc> live;

  // Insert some initial blocks.
  // Use 4-chunk aligned addresses to keep things simple.
  for (size_t i = 0; i < 64; i += 4)
  {
    address_t addr = chunk_addr(i);
    bc.add_fresh_range(addr, chunk_size(4));
  }

  // Do 1000 random alloc/free operations.
  for (size_t op = 0; op < 1000; op++)
  {
    if (live.size() > 0 && (rng() % 3 != 0))
    {
      // Free a random live allocation.
      size_t idx = rng() % live.size();
      bc.add_block(live[idx].addr, live[idx].size);
      live.erase(live.begin() + static_cast<ptrdiff_t>(idx));
    }
    else
    {
      // Allocate sizeclass 1 (always the smallest).
      auto result = bc.remove_block(chunk_size(1));
      if (result.addr != 0)
      {
        live.push_back({result.addr, result.size});
      }
    }
  }

  // Free everything remaining.
  for (auto& a : live)
  {
    bc.add_block(a.addr, a.size);
  }
  live.clear();

  // Verify we can allocate everything again as sizeclass 1.
  size_t total_freed = 0;
  for (;;)
  {
    auto result = bc.remove_block(chunk_size(1));
    if (result.addr == 0)
      break;
    total_freed += result.size;
  }

  // We initially inserted 16 blocks of 4 chunks each = 64 chunks.
  CHECK(total_freed == chunk_size(64));
}

// ---- Test: carving simulation ----
// Simulate the range wrapper's carving logic using the mock.

void test_carving_exact_fit()
{
  std::cout << "test_carving_exact_fit..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a 4-chunk block at chunk 4 (alpha=4).
  // This block exactly fits sizeclass 4 at alignment 4.
  address_t addr = chunk_addr(3); // 4*MCS
  bc.add_fresh_range(addr, chunk_size(4));

  auto result = bc.remove_block(chunk_size(4));
  CHECK(result.addr == addr);
  CHECK(result.size == chunk_size(4));

  // Carving: aligned_addr = align_up(addr, 4*MCS) = addr since addr is 4-aligned.
  address_t aligned = bits::align_up(result.addr, chunk_size(4));
  CHECK(aligned == addr);
  // No prefix, no suffix.
  size_t prefix = aligned - result.addr;
  size_t suffix = result.size - prefix - chunk_size(4);
  CHECK(prefix == 0);
  CHECK(suffix == 0);
}

void test_carving_with_prefix()
{
  std::cout << "test_carving_with_prefix..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a block at chunk 1 (alpha=1), size 8 chunks.
  // For sizeclass 4: align=4, T(4,1) = 7. n=8 >= 7 → can serve.
  // But the block address is 1*MCS which is not 4*MCS-aligned.
  address_t addr = chunk_addr(0); // 1*MCS
  bc.add_fresh_range(addr, chunk_size(8));

  auto result = bc.remove_block(chunk_size(4));
  CHECK(result.addr != 0);

  // Carving: aligned = align_up(1*MCS, 4*MCS) = 4*MCS
  address_t aligned = bits::align_up(result.addr, chunk_size(4));
  size_t prefix = aligned - result.addr;
  size_t suffix = result.size - prefix - chunk_size(4);

  CHECK(aligned == chunk_addr(3)); // 4*MCS
  CHECK(prefix == chunk_size(3));  // 3 chunks of prefix
  CHECK(suffix == chunk_size(1));  // 1 chunk of suffix

  // Return remainders to the pool.
  if (prefix > 0)
    bc.add_fresh_range(result.addr, prefix);
  if (suffix > 0)
    bc.add_fresh_range(aligned + chunk_size(4), suffix);

  // Should be able to allocate from the remainders.
  auto r2 = bc.remove_block(chunk_size(1));
  CHECK(r2.addr != 0);
}

void test_carving_with_suffix()
{
  std::cout << "test_carving_with_suffix..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a 16-chunk block at chunk 16 (alpha=16).
  // For sizeclass 4: aligned, no prefix, suffix = 12 chunks.
  address_t addr = chunk_addr(15); // 16*MCS
  bc.add_fresh_range(addr, chunk_size(16));

  auto result = bc.remove_block(chunk_size(4));
  CHECK(result.addr == addr);

  address_t aligned = bits::align_up(result.addr, chunk_size(4));
  size_t prefix = aligned - result.addr;
  size_t suffix = result.size - prefix - chunk_size(4);

  CHECK(aligned == addr);
  CHECK(prefix == 0);
  CHECK(suffix == chunk_size(12));

  // Return suffix.
  if (suffix > 0)
    bc.add_fresh_range(aligned + chunk_size(4), suffix);

  // Should be able to allocate from the suffix.
  auto r2 = bc.remove_block(chunk_size(1));
  CHECK(r2.addr != 0);
  CHECK(r2.size == chunk_size(12));
}

void test_carving_both()
{
  std::cout << "test_carving_both..." << std::endl;
  reset_mock();
  BCCore bc{};

  // Insert a 16-chunk block at chunk 2 (alpha=2).
  // For sizeclass 8: align=8, T(8,2) = 8+6 = 14. n=16 >= 14 → can serve.
  // aligned = align_up(2*MCS, 8*MCS) = 8*MCS. prefix = 6, suffix = 2.
  address_t addr = chunk_addr(1); // 2*MCS
  bc.add_fresh_range(addr, chunk_size(16));

  auto result = bc.remove_block(chunk_size(8));
  CHECK(result.addr != 0);

  address_t aligned = bits::align_up(result.addr, chunk_size(8));
  size_t prefix = aligned - result.addr;
  size_t suffix = result.size - prefix - chunk_size(8);

  CHECK(aligned == chunk_addr(7)); // 8*MCS
  CHECK(prefix == chunk_size(6));
  CHECK(suffix == chunk_size(2));

  // Verify the aligned address IS properly aligned.
  CHECK(aligned % chunk_size(8) == 0);
}

void test_carving_alignment_correctness()
{
  std::cout << "test_carving_alignment_correctness..." << std::endl;

  // For each valid sizeclass, insert a block with alpha=1 (worst case),
  // allocate, and verify the carved address has proper alignment.
  std::vector<size_t> test_sizes;
  test_sizes.push_back(1);
  test_sizes.push_back(2);
  test_sizes.push_back(3);
  for (size_t e = 2; e <= 5; e++)
  {
    for (size_t m = 0; m < BC::SL_COUNT; m++)
      test_sizes.push_back(BC::sizeclass_size(e, m));
  }

  for (size_t sc : test_sizes)
  {
    reset_mock();
    BCCore bc{};

    size_t sc_align = BC::natural_alignment(sc);

    // Compute threshold at alpha=1 to know how big the block needs to be.
    size_t thr = BC::threshold(sc, 1);
    // Use a block of size = thr at chunk 1 (alpha=1).
    if (thr > ARENA_CHUNKS)
      continue;

    address_t addr = chunk_addr(0); // 1*MCS, alpha=1
    bc.add_fresh_range(addr, chunk_size(thr));

    auto result = bc.remove_block(chunk_size(sc));
    if (result.addr == 0)
    {
      std::cout << "  FAIL: sc=" << sc << " thr=" << thr
                << " could not allocate" << std::endl;
      CHECK(false);
      continue;
    }

    address_t aligned = bits::align_up(result.addr, sc_align * MIN_CHUNK_SIZE);
    CHECK(aligned + chunk_size(sc) <= result.addr + result.size);
    CHECK(aligned % (sc_align * MIN_CHUNK_SIZE) == 0);
  }
}

// ---- Test: round_up_sizeclass ----

void test_round_up_sizeclass()
{
  std::cout << "test_round_up_sizeclass..." << std::endl;

  CHECK(BC::round_up_sizeclass(0) == 0);
  CHECK(BC::round_up_sizeclass(1) == 1);
  CHECK(BC::round_up_sizeclass(2) == 2);
  CHECK(BC::round_up_sizeclass(3) == 3);
  CHECK(BC::round_up_sizeclass(4) == 4);

  // 5 is already valid (e=2, m=1)
  CHECK(BC::round_up_sizeclass(5) == 5);
  // 9 is not valid; rounds to 10
  CHECK(BC::round_up_sizeclass(9) == 10);
  // 11 rounds to 12
  CHECK(BC::round_up_sizeclass(11) == 12);
  // 15 rounds to 16
  CHECK(BC::round_up_sizeclass(15) == 16);
  // 16 is already valid
  CHECK(BC::round_up_sizeclass(16) == 16);

  // Exhaustive: for all n from 1 to 256, round_up is >= n and is valid.
  for (size_t n = 1; n <= 256; n++)
  {
    size_t r = BC::round_up_sizeclass(n);
    CHECK(r >= n);
    CHECK(BC::is_valid_sizeclass(r));
    // And it's the smallest valid sizeclass >= n.
    if (r > n)
    {
      for (size_t k = n; k < r; k++)
        CHECK(!BC::is_valid_sizeclass(k));
    }
  }
}

// ---- Main ----

int main()
{
  setup();

  test_insert_remove_roundtrip();
  test_bitmap_correctness();
  test_multiple_bins();
  test_best_fit_search();
  test_masked_search();
  test_remove_from_bin();
  test_empty_return();
  test_boundary_tags();
  test_single_chunk();
  test_large_serves_small();
  test_coalesce_right();
  test_coalesce_left();
  test_coalesce_both();
  test_boundary_prevents_coalescing();
  test_stale_tag_after_remove();
  test_stale_tag_after_absorption();
  test_left_walk_stops_at_boundary();
  test_stress();
  test_carving_exact_fit();
  test_carving_with_prefix();
  test_carving_with_suffix();
  test_carving_both();
  test_carving_alignment_correctness();
  test_round_up_sizeclass();

  if (failure_count > 0)
  {
    std::cout << "\n" << failure_count << " FAILURES" << std::endl;
    return 1;
  }

  std::cout << "\nAll bc_core tests passed." << std::endl;
  return 0;
}
