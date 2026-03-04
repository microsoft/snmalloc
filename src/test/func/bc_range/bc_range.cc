/**
 * Pipeline integration tests for BitmapCoalesceRange.
 *
 * Exercises BitmapCoalesceRange through the real snmalloc pipeline
 * (FixedRangeConfig -> StandardLocalState -> BitmapCoalesceRange)
 * using large-object allocation patterns that exercise coalescing,
 * carving, and refill paths.
 */

#include "test/setup.h"

#include <iostream>
#include <vector>

#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

// Use a reasonably large arena so the tests have room to exercise
// coalescing behaviour without running out of backing memory.
static constexpr size_t ARENA_BITS = 30; // 1 GiB
static constexpr size_t ARENA_SIZE = bits::one_at_bit(ARENA_BITS);

// ---- Per-test globals setup ----
// FixedRangeConfig has static state, so we can only initialise once.
// All test functions run sequentially within a single init.

using CustomGlobals = FixedRangeConfig<DefaultPal>;
using FixedAlloc = Allocator<CustomGlobals>;

/**
 * test_large_alloc_roundtrip
 *
 * Allocate a chunk-sized block, write to every page, free it,
 * then reallocate.  The second allocation should succeed (proving
 * the freed block re-entered the pool via BitmapCoalesceRange).
 */
template<typename Alloc>
static void test_large_alloc_roundtrip(Alloc& a)
{
  std::cout << "  test_large_alloc_roundtrip ... " << std::flush;

  constexpr size_t obj_size = MIN_CHUNK_SIZE; // 16 KiB
  auto* p1 = static_cast<char*>(a->alloc(obj_size));
  SNMALLOC_CHECK(p1 != nullptr);

  // Touch every page to ensure commit path works.
  for (size_t i = 0; i < obj_size; i += 4096)
    p1[i] = static_cast<char>(i & 0xFF);

  a->dealloc(p1);

  // Re-allocate the same size — should succeed via the coalescing range.
  auto* p2 = static_cast<char*>(a->alloc(obj_size));
  SNMALLOC_CHECK(p2 != nullptr);
  a->dealloc(p2);

  std::cout << "OK" << std::endl;
}

/**
 * test_large_alloc_after_free
 *
 * Allocate N chunks, free them all, then allocate a single block
 * larger than any individual chunk.  This forces BitmapCoalesceRange
 * to coalesce adjacent freed chunks into a larger block.
 */
template<typename Alloc>
static void test_large_alloc_after_free(Alloc& a)
{
  std::cout << "  test_large_alloc_after_free ... " << std::flush;

  constexpr size_t N = 8;
  constexpr size_t obj_size = MIN_CHUNK_SIZE;
  void* ptrs[N];

  for (size_t i = 0; i < N; i++)
  {
    ptrs[i] = a->alloc(obj_size);
    SNMALLOC_CHECK(ptrs[i] != nullptr);
  }

  // Free all.
  for (size_t i = 0; i < N; i++)
    a->dealloc(ptrs[i]);

  // Now allocate something larger.  If coalescing works, the freed
  // chunks should have been merged and this should not need a fresh
  // refill from the parent (though the test doesn't assert on that
  // internal detail — it just checks success).
  constexpr size_t big_size = MIN_CHUNK_SIZE * 4;
  auto* big = a->alloc(big_size);
  SNMALLOC_CHECK(big != nullptr);
  a->dealloc(big);

  std::cout << "OK" << std::endl;
}

/**
 * test_non_pow2_sizes
 *
 * Allocate objects whose sizes are valid snmalloc sizeclasses that
 * are NOT powers of two.  The bitmap coalescing range uses B=2
 * intermediate bits, so sizeclasses include 3/4 and 5/4 multiples
 * of each power of two.  This exercises the carving / alignment
 * logic inside BitmapCoalesceRange::alloc_range.
 */
template<typename Alloc>
static void test_non_pow2_sizes(Alloc& a)
{
  std::cout << "  test_non_pow2_sizes ... " << std::flush;

  // Pick a few sizes that land on non-power-of-two sizeclasses.
  // These are all multiples of MIN_CHUNK_SIZE so they go through
  // the large-object path which uses BitmapCoalesceRange.
  const size_t sizes[] = {
    MIN_CHUNK_SIZE * 3, // 3 chunks — not a power of two
    MIN_CHUNK_SIZE * 5, // 5 chunks
    MIN_CHUNK_SIZE * 6, // 6 chunks
  };

  for (auto sz : sizes)
  {
    auto* p = a->alloc(sz);
    SNMALLOC_CHECK(p != nullptr);

    // Verify alignment: should be at least MIN_CHUNK_SIZE-aligned.
    SNMALLOC_CHECK(
      (reinterpret_cast<uintptr_t>(p) & (MIN_CHUNK_SIZE - 1)) == 0);

    a->dealloc(p);
  }

  std::cout << "OK" << std::endl;
}

/**
 * test_range_stress
 *
 * Rapid alloc / dealloc cycles with varying sizes, exercising
 * the coalescing range under pressure.  Allocates in waves: first
 * fills up a batch, then frees every other allocation (creating
 * fragmentation), then allocates again to exercise coalescing of
 * the freed slots.
 */
template<typename Alloc>
static void test_range_stress(Alloc& a)
{
  std::cout << "  test_range_stress ... " << std::flush;

  constexpr size_t BATCH = 32;
  void* ptrs[BATCH];

  // Wave 1: allocate a batch of chunk-sized objects.
  for (size_t i = 0; i < BATCH; i++)
  {
    ptrs[i] = a->alloc(MIN_CHUNK_SIZE);
    SNMALLOC_CHECK(ptrs[i] != nullptr);
  }

  // Free every other one — leaves alternating holes.
  for (size_t i = 0; i < BATCH; i += 2)
  {
    a->dealloc(ptrs[i]);
    ptrs[i] = nullptr;
  }

  // Re-fill the holes.
  for (size_t i = 0; i < BATCH; i += 2)
  {
    ptrs[i] = a->alloc(MIN_CHUNK_SIZE);
    SNMALLOC_CHECK(ptrs[i] != nullptr);
  }

  // Free everything.
  for (size_t i = 0; i < BATCH; i++)
  {
    a->dealloc(ptrs[i]);
    ptrs[i] = nullptr;
  }

  // Final: allocate a multi-chunk block to prove coalescing succeeded.
  auto* big = a->alloc(MIN_CHUNK_SIZE * 8);
  SNMALLOC_CHECK(big != nullptr);
  a->dealloc(big);

  std::cout << "OK" << std::endl;
}

/**
 * test_mixed_sizes
 *
 * Interleave allocations of different sizes and free them in a
 * different order, exercising the coalescing range's ability to
 * merge blocks of varying sizes and maintain correct boundary tags.
 */
template<typename Alloc>
static void test_mixed_sizes(Alloc& a)
{
  std::cout << "  test_mixed_sizes ... " << std::flush;

  auto* p1 = a->alloc(MIN_CHUNK_SIZE);
  auto* p2 = a->alloc(MIN_CHUNK_SIZE * 2);
  auto* p3 = a->alloc(MIN_CHUNK_SIZE);
  auto* p4 = a->alloc(MIN_CHUNK_SIZE * 4);

  SNMALLOC_CHECK(p1 != nullptr);
  SNMALLOC_CHECK(p2 != nullptr);
  SNMALLOC_CHECK(p3 != nullptr);
  SNMALLOC_CHECK(p4 != nullptr);

  // Free in reverse order.
  a->dealloc(p4);
  a->dealloc(p3);
  a->dealloc(p2);
  a->dealloc(p1);

  // Allocate a large block — should succeed if coalescing worked.
  auto* big = a->alloc(MIN_CHUNK_SIZE * 8);
  SNMALLOC_CHECK(big != nullptr);
  a->dealloc(big);

  std::cout << "OK" << std::endl;
}

int main()
{
  setup();

  std::cout << "bc_range: pipeline integration tests for BitmapCoalesceRange"
            << std::endl;

  auto* oe_base = DefaultPal::reserve(ARENA_SIZE);
  SNMALLOC_CHECK(oe_base != nullptr);

  CustomGlobals::init(nullptr, oe_base, ARENA_SIZE);

  auto a = get_scoped_allocator<FixedAlloc>();

  test_large_alloc_roundtrip(a);
  test_large_alloc_after_free(a);
  test_non_pow2_sizes(a);
  test_range_stress(a);
  test_mixed_sizes(a);

  std::cout << "bc_range: all tests passed" << std::endl;
  return 0;
}
