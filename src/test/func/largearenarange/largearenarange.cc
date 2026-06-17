/**
 * Unit tests for LargeArenaRange and PagemapRep.
 *
 * Tests the Range wrapper around Arena using a real pagemap,
 * exercising alloc_range, dealloc_range, refill, and overflow paths.
 */

#include "test/setup.h"

#include <cstdio>

#ifndef SNMALLOC_TRACING
#  define SNMALLOC_TRACING
#endif
#include "test/snmalloc_testlib.h"

#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/snmalloc_core.h>

namespace
{
  using namespace snmalloc;

  // --- Test pagemap and range types ---

  using Pal = DefaultPal;
  using PagemapEntry = DefaultPagemapEntry<NoClientMetaDataProvider>;
  using ConcretePagemap = FlatPagemap<MIN_CHUNK_BITS, PagemapEntry, Pal, false>;
  using TestPagemap = BasicPagemap<Pal, ConcretePagemap, PagemapEntry, false>;

  // Initialise the pagemap once before tests.
  static bool pagemap_initialised = false;

  static void ensure_pagemap()
  {
    if (!pagemap_initialised)
    {
      TestPagemap::concretePagemap.template init<false>();
      pagemap_initialised = true;
    }
  }

  // Simple parent: PalRange + PagemapRegisterRange.
  using ParentSource = Pipe<PalRange<Pal>, PagemapRegisterRange<TestPagemap>>;

  // LargeArenaRange under test: global range (MAX_SIZE_BITS = BITS - 1).
  // This means overflow dealloc never goes to parent (matches the global
  // range configuration). MIN_REFILL_BITS = MinBaseSizeBits<Pal>() so
  // the first parent allocation is at least the PAL's minimum reserve
  // size — Windows VirtualAlloc cannot reserve below its allocation
  // granularity (64 KiB) and PalRange returns nullptr in that case.
  static constexpr size_t REFILL_BITS = 20;
  static constexpr size_t MAX_BITS = bits::BITS - 1;
  static constexpr size_t MIN_REFILL_BITS = MinBaseSizeBits<Pal>();

  using ArenaRange = Pipe<
    ParentSource,
    LargeArenaRange<REFILL_BITS, MAX_BITS, TestPagemap, MIN_REFILL_BITS>>;

  // --- Tests ---

  static void test_basic_alloc_dealloc()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Allocate a single chunk.
    auto p1 = range.alloc_range(MIN_CHUNK_SIZE);
    SNMALLOC_ASSERT(p1 != nullptr);
    printf("  alloc %zu bytes at %p\n", MIN_CHUNK_SIZE, p1.unsafe_ptr());

    // Deallocate and re-allocate — should succeed.
    range.dealloc_range(p1, MIN_CHUNK_SIZE);
    auto p2 = range.alloc_range(MIN_CHUNK_SIZE);
    SNMALLOC_ASSERT(p2 != nullptr);

    // Clean up.
    range.dealloc_range(p2, MIN_CHUNK_SIZE);

    printf("  Basic alloc/dealloc: OK\n");
  }

  static void test_multiple_sizes()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Allocate various power-of-two sizes.
    constexpr size_t NUM_SIZES = 6;
    size_t sizes[NUM_SIZES] = {
      MIN_CHUNK_SIZE,
      MIN_CHUNK_SIZE * 2,
      MIN_CHUNK_SIZE * 4,
      MIN_CHUNK_SIZE * 8,
      MIN_CHUNK_SIZE * 16,
      MIN_CHUNK_SIZE * 32};
    capptr::Arena<void> ptrs[NUM_SIZES] = {};

    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      ptrs[i] = range.alloc_range(sizes[i]);
      SNMALLOC_ASSERT(ptrs[i] != nullptr);
    }

    // Deallocate all.
    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      range.dealloc_range(ptrs[i], sizes[i]);
    }

    printf("  Multiple sizes: OK\n");
  }

  static void test_refill()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Allocate more than one refill's worth of chunks.
    // REFILL_SIZE is 2^20, MIN_CHUNK_SIZE is 2^14,
    // so one refill is ~64 chunks.
    constexpr size_t NUM_ALLOCS = 200;
    capptr::Arena<void> ptrs[NUM_ALLOCS] = {};

    for (size_t i = 0; i < NUM_ALLOCS; i++)
    {
      ptrs[i] = range.alloc_range(MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(ptrs[i] != nullptr);
    }

    // Deallocate all.
    for (size_t i = 0; i < NUM_ALLOCS; i++)
    {
      range.dealloc_range(ptrs[i], MIN_CHUNK_SIZE);
    }

    // Re-allocate — should serve from freed blocks, no new refill needed
    // for the first pass.
    for (size_t i = 0; i < NUM_ALLOCS; i++)
    {
      ptrs[i] = range.alloc_range(MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(ptrs[i] != nullptr);
    }

    // Final cleanup.
    for (size_t i = 0; i < NUM_ALLOCS; i++)
    {
      range.dealloc_range(ptrs[i], MIN_CHUNK_SIZE);
    }

    printf("  Refill (200 allocs): OK\n");
  }

  static void test_alloc_dealloc_cycle()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Interleave alloc and dealloc to exercise consolidation.
    constexpr size_t ROUNDS = 100;
    for (size_t r = 0; r < ROUNDS; r++)
    {
      auto p = range.alloc_range(MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(p != nullptr);
      range.dealloc_range(p, MIN_CHUNK_SIZE);
    }

    // Do a larger allocation after many cycles — verifies
    // that consolidation is working (freed chunks merge back).
    auto large = range.alloc_range(MIN_CHUNK_SIZE * 4);
    SNMALLOC_ASSERT(large != nullptr);
    range.dealloc_range(large, MIN_CHUNK_SIZE * 4);

    printf("  Alloc/dealloc cycle: OK\n");
  }

  static void test_alignment()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Verify that returned pointers are properly aligned.
    constexpr size_t NUM_TESTS = 5;
    size_t sizes[NUM_TESTS] = {
      MIN_CHUNK_SIZE,
      MIN_CHUNK_SIZE * 2,
      MIN_CHUNK_SIZE * 4,
      MIN_CHUNK_SIZE * 8,
      MIN_CHUNK_SIZE * 16};

    for (size_t i = 0; i < NUM_TESTS; i++)
    {
      auto p = range.alloc_range(sizes[i]);
      SNMALLOC_ASSERT(p != nullptr);
      uintptr_t addr = p.unsafe_uintptr();
      SNMALLOC_ASSERT(
        (addr & (sizes[i] - 1)) == 0 && "Allocation not properly aligned");
      UNUSED(addr);
      range.dealloc_range(p, sizes[i]);
    }

    printf("  Alignment: OK\n");
  }

  static void test_large_then_small()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Allocate a large block, dealloc, then allocate smaller blocks
    // from the same space.
    size_t large_size = MIN_CHUNK_SIZE * 16;
    auto large = range.alloc_range(large_size);
    SNMALLOC_ASSERT(large != nullptr);
    range.dealloc_range(large, large_size);

    // Now allocate 16 individual chunks — should come from the freed
    // large block's space.
    constexpr size_t N = 16;
    capptr::Arena<void> ptrs[N] = {};
    for (size_t i = 0; i < N; i++)
    {
      ptrs[i] = range.alloc_range(MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(ptrs[i] != nullptr);
    }

    for (size_t i = 0; i < N; i++)
    {
      range.dealloc_range(ptrs[i], MIN_CHUNK_SIZE);
    }

    printf("  Large then small: OK\n");
  }

  static void test_non_pow2_sizes()
  {
    ensure_pagemap();
    ArenaRange range{};

    // Non-power-of-two, chunk-multiple sizes. Some of these are not
    // representable size-classes (e.g. 9, 11, 13 chunks); the arena
    // carves exactly the requested chunk count and rolls the rounding
    // remainder into the post fragment, so callers see no excess.
    constexpr size_t NUM_SIZES = 8;
    size_t sizes[NUM_SIZES] = {
      MIN_CHUNK_SIZE * 3,
      MIN_CHUNK_SIZE * 5,
      MIN_CHUNK_SIZE * 6,
      MIN_CHUNK_SIZE * 7,
      MIN_CHUNK_SIZE * 9,
      MIN_CHUNK_SIZE * 11,
      MIN_CHUNK_SIZE * 13,
      MIN_CHUNK_SIZE * 17};

    capptr::Arena<void> ptrs[NUM_SIZES] = {};
    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      ptrs[i] = range.alloc_range(sizes[i]);
      SNMALLOC_ASSERT(ptrs[i] != nullptr);
    }

    // All pointers must be distinct and non-overlapping (within the size
    // requested — over-allocation would break this because the rounding
    // remainder would later be handed out a second time).
    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      uintptr_t lo_i = ptrs[i].unsafe_uintptr();
      uintptr_t hi_i = lo_i + sizes[i];
      for (size_t j = i + 1; j < NUM_SIZES; j++)
      {
        uintptr_t lo_j = ptrs[j].unsafe_uintptr();
        uintptr_t hi_j = lo_j + sizes[j];
        SNMALLOC_ASSERT(hi_i <= lo_j || hi_j <= lo_i);
        UNUSED(hi_i, hi_j);
      }
    }

    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      range.dealloc_range(ptrs[i], sizes[i]);
    }

    // After deallocating all, repeat the exact same pattern to confirm
    // the freed space is reusable (catches leaks from un-returned
    // rounding remainder).
    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      ptrs[i] = range.alloc_range(sizes[i]);
      SNMALLOC_ASSERT(ptrs[i] != nullptr);
    }
    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      range.dealloc_range(ptrs[i], sizes[i]);
    }

    printf("  Non-pow2 sizes: OK\n");
  }
} // anonymous namespace

int main()
{
  setup();

  printf("--- LargeArenaRange tests ---\n");

  test_basic_alloc_dealloc();
  test_multiple_sizes();
  test_refill();
  test_alloc_dealloc_cycle();
  test_alignment();
  test_large_then_small();
  test_non_pow2_sizes();

  printf("All LargeArenaRange tests passed.\n");
  return 0;
}
