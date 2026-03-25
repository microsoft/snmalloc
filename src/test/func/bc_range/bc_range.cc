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
#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc.h>
#include <vector>

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
 * Assert that the pointer is naturally aligned for the given allocation size.
 * Natural alignment = largest power of 2 dividing the size.
 */
static void check_natural_alignment(void* p, size_t size, const char* context)
{
  size_t nat_align = natural_alignment(size);
  auto addr = reinterpret_cast<uintptr_t>(p);
  if ((addr % nat_align) != 0)
  {
    std::cout << "\n  FAIL [" << context << "]: alloc(" << size << ") returned "
              << p << " not aligned to " << nat_align << " (offset "
              << (addr % nat_align) << ")" << std::endl;
  }
  SNMALLOC_CHECK((addr % nat_align) == 0);
}

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
  check_natural_alignment(p1, obj_size, "roundtrip p1");

  // Touch every page to ensure commit path works.
  for (size_t i = 0; i < obj_size; i += 4096)
    p1[i] = static_cast<char>(i & 0xFF);

  a->dealloc(p1);

  // Re-allocate the same size — should succeed via the coalescing range.
  auto* p2 = static_cast<char*>(a->alloc(obj_size));
  SNMALLOC_CHECK(p2 != nullptr);
  check_natural_alignment(p2, obj_size, "roundtrip p2");
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
    check_natural_alignment(ptrs[i], obj_size, "alloc_after_free batch");
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
  check_natural_alignment(big, big_size, "alloc_after_free big");
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
 *
 * Verifies natural alignment: alloc_range must return addresses
 * aligned to the largest power of two dividing the allocation size.
 */
template<typename Alloc>
static void test_non_pow2_sizes(Alloc& a)
{
  std::cout << "  test_non_pow2_sizes ... " << std::flush;

  // Pick a few sizes that land on non-power-of-two sizeclasses.
  // These are all multiples of MIN_CHUNK_SIZE so they go through
  // the large-object path which uses BitmapCoalesceRange.
  const size_t sizes[] = {
    MIN_CHUNK_SIZE * 3, // 3 chunks — natural alignment 1 chunk
    MIN_CHUNK_SIZE * 5, // 5 chunks — natural alignment 1 chunk
    MIN_CHUNK_SIZE * 6, // 6 chunks — natural alignment 2 chunks
  };

  for (auto sz : sizes)
  {
    auto* p = a->alloc(sz);
    SNMALLOC_CHECK(p != nullptr);

    // Verify natural alignment: address must be divisible by the
    // largest power of two dividing the allocation size.
    check_natural_alignment(p, sz, "non_pow2");

    a->dealloc(p);
  }

  std::cout << "OK" << std::endl;
}

/**
 * test_alignment_after_fragmentation
 *
 * Randomly allocates and deallocates a mix of pow2 and non-pow2
 * large sizes to build up fragmentation, then verifies that every
 * allocation is naturally aligned.
 *
 * Uses a simple xorshift PRNG for reproducibility.
 */
template<typename Alloc>
static void test_alignment_after_fragmentation(Alloc& a)
{
  std::cout << "  test_alignment_after_fragmentation ... " << std::flush;

  static constexpr size_t MAX_LIVE = 128;
  static constexpr size_t NUM_OPS = 2000;

  // All sizes are valid bitmap-coalesce sizeclasses (multiples of
  // MIN_CHUNK_SIZE with chunk counts from the B=2 sequence).
  const size_t chunk_counts[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 28, 32};
  constexpr size_t N_SIZES = sizeof(chunk_counts) / sizeof(chunk_counts[0]);

  struct Slot
  {
    void* ptr;
    size_t size;
  };

  Slot live[MAX_LIVE] = {};
  size_t num_live = 0;
  uint32_t rng = 42;

  auto xorshift = [&]() -> uint32_t {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  };

  for (size_t op = 0; op < NUM_OPS; op++)
  {
    bool do_alloc;
    if (num_live == 0)
      do_alloc = true;
    else if (num_live >= MAX_LIVE)
      do_alloc = false;
    else
      do_alloc = (xorshift() % 3) != 0; // 2/3 alloc, 1/3 free

    if (do_alloc)
    {
      size_t chunks = chunk_counts[xorshift() % N_SIZES];
      size_t sz = chunks * MIN_CHUNK_SIZE;

      auto* p = a->alloc(sz);
      SNMALLOC_CHECK(p != nullptr);

      // Verify natural alignment.
      check_natural_alignment(p, sz, "fragmentation");

      live[num_live].ptr = p;
      live[num_live].size = sz;
      num_live++;
    }
    else
    {
      size_t idx = xorshift() % num_live;
      a->dealloc(live[idx].ptr);
      live[idx] = live[num_live - 1];
      num_live--;
    }
  }

  // Free remaining.
  for (size_t i = 0; i < num_live; i++)
    a->dealloc(live[i].ptr);

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
    check_natural_alignment(ptrs[i], MIN_CHUNK_SIZE, "stress wave1");
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
    check_natural_alignment(ptrs[i], MIN_CHUNK_SIZE, "stress refill");
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
  check_natural_alignment(big, MIN_CHUNK_SIZE * 8, "stress big");
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

  check_natural_alignment(p1, MIN_CHUNK_SIZE, "mixed p1");
  check_natural_alignment(p2, MIN_CHUNK_SIZE * 2, "mixed p2");
  check_natural_alignment(p3, MIN_CHUNK_SIZE, "mixed p3");
  check_natural_alignment(p4, MIN_CHUNK_SIZE * 4, "mixed p4");

  // Free in reverse order.
  a->dealloc(p4);
  a->dealloc(p3);
  a->dealloc(p2);
  a->dealloc(p1);

  // Allocate a large block — should succeed if coalescing worked.
  auto* big = a->alloc(MIN_CHUNK_SIZE * 8);
  SNMALLOC_CHECK(big != nullptr);
  check_natural_alignment(big, MIN_CHUNK_SIZE * 8, "mixed big");
  a->dealloc(big);

  std::cout << "OK" << std::endl;
}

/**
 * test_alignment_after_dealloc_sequences
 *
 * Allocates blocks of various sizes, frees them in different orders,
 * then re-allocates and checks that every result is naturally aligned.
 * Exercises the coalescing path's ability to produce correctly aligned
 * blocks from merged free regions.
 */
template<typename Alloc>
static void test_alignment_after_dealloc_sequences(Alloc& a)
{
  std::cout << "  test_alignment_after_dealloc_sequences ... " << std::flush;

  // Sizes in chunks — mix of pow2 and non-pow2 to stress alignment.
  const size_t chunk_counts[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16};
  constexpr size_t N = sizeof(chunk_counts) / sizeof(chunk_counts[0]);

  // --- Sequence 1: allocate all, free all forward, re-allocate ---
  {
    void* ptrs[N];
    size_t sizes[N];
    for (size_t i = 0; i < N; i++)
    {
      sizes[i] = chunk_counts[i] * MIN_CHUNK_SIZE;
      ptrs[i] = a->alloc(sizes[i]);
      SNMALLOC_CHECK(ptrs[i] != nullptr);
      check_natural_alignment(ptrs[i], sizes[i], "dealloc_seq1 alloc");
    }
    for (size_t i = 0; i < N; i++)
      a->dealloc(ptrs[i]);
    // Re-allocate in reverse size order after forward free.
    for (size_t i = N; i > 0; i--)
    {
      ptrs[i - 1] = a->alloc(sizes[i - 1]);
      SNMALLOC_CHECK(ptrs[i - 1] != nullptr);
      check_natural_alignment(
        ptrs[i - 1], sizes[i - 1], "dealloc_seq1 realloc");
    }
    for (size_t i = 0; i < N; i++)
      a->dealloc(ptrs[i]);
  }

  // --- Sequence 2: allocate all, free all reverse, re-allocate ---
  {
    void* ptrs[N];
    size_t sizes[N];
    for (size_t i = 0; i < N; i++)
    {
      sizes[i] = chunk_counts[i] * MIN_CHUNK_SIZE;
      ptrs[i] = a->alloc(sizes[i]);
      SNMALLOC_CHECK(ptrs[i] != nullptr);
      check_natural_alignment(ptrs[i], sizes[i], "dealloc_seq2 alloc");
    }
    for (size_t i = N; i > 0; i--)
      a->dealloc(ptrs[i - 1]);
    for (size_t i = 0; i < N; i++)
    {
      ptrs[i] = a->alloc(sizes[i]);
      SNMALLOC_CHECK(ptrs[i] != nullptr);
      check_natural_alignment(ptrs[i], sizes[i], "dealloc_seq2 realloc");
    }
    for (size_t i = 0; i < N; i++)
      a->dealloc(ptrs[i]);
  }

  // --- Sequence 3: interleaved alloc/dealloc with growing sizes ---
  {
    void* prev = nullptr;
    for (size_t i = 0; i < N; i++)
    {
      size_t sz = chunk_counts[i] * MIN_CHUNK_SIZE;
      void* p = a->alloc(sz);
      SNMALLOC_CHECK(p != nullptr);
      check_natural_alignment(p, sz, "dealloc_seq3 interleave");

      if (prev != nullptr)
        a->dealloc(prev);

      prev = p;
    }
    if (prev != nullptr)
      a->dealloc(prev);

    // Now allocate a large block from the coalesced space.
    size_t big_sz = MIN_CHUNK_SIZE * 16;
    void* big = a->alloc(big_sz);
    SNMALLOC_CHECK(big != nullptr);
    check_natural_alignment(big, big_sz, "dealloc_seq3 big");
    a->dealloc(big);
  }

  // --- Sequence 4: free odd-indexed, then even-indexed, re-allocate larger ---
  {
    void* ptrs[N];
    size_t sizes[N];
    for (size_t i = 0; i < N; i++)
    {
      sizes[i] = chunk_counts[i] * MIN_CHUNK_SIZE;
      ptrs[i] = a->alloc(sizes[i]);
      SNMALLOC_CHECK(ptrs[i] != nullptr);
      check_natural_alignment(ptrs[i], sizes[i], "dealloc_seq4 alloc");
    }
    // Free odd indices first.
    for (size_t i = 1; i < N; i += 2)
      a->dealloc(ptrs[i]);
    // Free even indices.
    for (size_t i = 0; i < N; i += 2)
      a->dealloc(ptrs[i]);
    // Re-allocate with different sizes.
    const size_t realloc_chunks[] = {2, 4, 6, 8, 3, 5, 1, 10, 12, 16};
    for (size_t i = 0; i < N; i++)
    {
      sizes[i] = realloc_chunks[i] * MIN_CHUNK_SIZE;
      ptrs[i] = a->alloc(sizes[i]);
      SNMALLOC_CHECK(ptrs[i] != nullptr);
      check_natural_alignment(ptrs[i], sizes[i], "dealloc_seq4 realloc");
    }
    for (size_t i = 0; i < N; i++)
      a->dealloc(ptrs[i]);
  }

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
  test_alignment_after_fragmentation(a);
  test_range_stress(a);
  test_mixed_sizes(a);
  test_alignment_after_dealloc_sequences(a);

  std::cout << "bc_range: all tests passed" << std::endl;
  return 0;
}
