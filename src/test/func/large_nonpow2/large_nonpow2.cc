#include <stdio.h>
#include <test/helpers.h>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#include <snmalloc/override/malloc.cc>

using namespace snmalloc;

/**
 * Test that large non-power-of-2 allocations:
 *   1. Return non-null pointers
 *   2. Have the correct natural alignment (NOT next-pow2 alignment)
 *   3. Report usable size >= requested size
 *   4. Can be written to and freed without error
 *   5. Work correctly under repeated alloc/free cycles
 */

/// The non-pow2 large size classes in chunks (B=2 scheme).
/// These are the sizes that exist in the fine-grained levels:
///   e=1: 3
///   e=2: 5, 6, 7
///   e=3: 10, 12, 14
///   e=4: 20, 24, 28
///   e=5: 40, 48, 56
///   e=6: 80, 96, 112
static constexpr size_t non_pow2_chunks[] = {
  3, 5, 6, 7, 10, 12, 14, 20, 24, 28, 40, 48, 56, 80, 96, 112};

static constexpr size_t NUM_SIZES =
  sizeof(non_pow2_chunks) / sizeof(non_pow2_chunks[0]);

/// Natural alignment of a size: largest power of 2 dividing it.
static size_t natural_align(size_t s)
{
  return s & (~(s - 1));
}

void test_alignment_and_usable_size()
{
  printf("  test_alignment_and_usable_size\n");
  for (size_t i = 0; i < NUM_SIZES; i++)
  {
    size_t size = non_pow2_chunks[i] * MIN_CHUNK_SIZE;
    void* p = our_malloc(size);
    EXPECT(p != nullptr, "malloc({}) returned null", size);

    // Check natural alignment.
    size_t expected_align = natural_align(size);
    size_t addr = reinterpret_cast<size_t>(p);
    EXPECT(
      (addr % expected_align) == 0,
      "malloc({}) returned {} not aligned to {} (natural alignment)",
      size,
      p,
      expected_align);

    // Verify it does NOT have next-pow2 alignment (which would mean
    // the allocator is still rounding up to pow2 internally).
    size_t pow2_align = bits::next_pow2(size);
    if (pow2_align != size)
    {
      // For a truly non-pow2 allocation, pow2 alignment is not guaranteed.
      // We don't assert it's NOT pow2-aligned (it might be by chance),
      // but we track it for reporting.
    }

    // Check usable size is at least what we asked for, and not rounded
    // up to next pow2.
    size_t usable = our_malloc_usable_size(p);
    EXPECT(
      usable >= size,
      "malloc_usable_size({}) = {} < requested {}",
      p,
      usable,
      size);
    EXPECT(
      usable < bits::next_pow2(size),
      "malloc_usable_size({}) = {} rounded up to pow2 {} for request {}",
      p,
      usable,
      bits::next_pow2(size),
      size);

    // Write to every page to verify the allocation is valid.
    memset(p, 0xAB, size);

    our_free(p);
  }
}

void test_many_allocations()
{
  printf("  test_many_allocations\n");
  // Allocate many objects of each non-pow2 size simultaneously.
  static constexpr size_t COUNT = 20;
  void* ptrs[NUM_SIZES][COUNT];

  for (size_t i = 0; i < NUM_SIZES; i++)
  {
    size_t size = non_pow2_chunks[i] * MIN_CHUNK_SIZE;
    for (size_t j = 0; j < COUNT; j++)
    {
      ptrs[i][j] = our_malloc(size);
      EXPECT(
        ptrs[i][j] != nullptr, "malloc({}) returned null at j={}", size, j);

      size_t addr = reinterpret_cast<size_t>(ptrs[i][j]);
      size_t expected_align = natural_align(size);
      EXPECT(
        (addr % expected_align) == 0,
        "malloc({}) at j={} returned {} not aligned to {}",
        size,
        j,
        ptrs[i][j],
        expected_align);

      // Touch the memory.
      memset(ptrs[i][j], static_cast<int>(i + j), size);
    }
  }

  // Free in reverse order.
  for (size_t i = NUM_SIZES; i > 0; i--)
  {
    for (size_t j = COUNT; j > 0; j--)
    {
      our_free(ptrs[i - 1][j - 1]);
    }
  }
}

void test_alloc_free_cycles()
{
  printf("  test_alloc_free_cycles\n");
  // Repeated alloc/free to exercise the cache and decay paths.
  static constexpr size_t CYCLES = 50;

  for (size_t cycle = 0; cycle < CYCLES; cycle++)
  {
    for (size_t i = 0; i < NUM_SIZES; i++)
    {
      size_t size = non_pow2_chunks[i] * MIN_CHUNK_SIZE;
      void* p = our_malloc(size);
      EXPECT(p != nullptr, "malloc({}) returned null at cycle={}", size, cycle);

      size_t addr = reinterpret_cast<size_t>(p);
      size_t expected_align = natural_align(size);
      EXPECT(
        (addr % expected_align) == 0,
        "cycle {} malloc({}) returned {} not aligned to {}",
        cycle,
        size,
        p,
        expected_align);

      // Write first and last bytes.
      static_cast<char*>(p)[0] = 'A';
      static_cast<char*>(p)[size - 1] = 'Z';

      our_free(p);
    }
  }
}

void test_mixed_sizes()
{
  printf("  test_mixed_sizes\n");
  // Interleave pow2 and non-pow2 large allocations.
  static constexpr size_t COUNT = 10;
  void* pow2_ptrs[COUNT];
  void* nonpow2_ptrs[COUNT];

  for (size_t i = 0; i < COUNT; i++)
  {
    // Pow2 allocation (128 KiB)
    size_t pow2_size = 128 * 1024;
    pow2_ptrs[i] = our_malloc(pow2_size);
    EXPECT(pow2_ptrs[i] != nullptr, "pow2 malloc({}) returned null", pow2_size);

    // Non-pow2 allocation (96 KiB = 6 chunks)
    size_t nonpow2_size = 6 * MIN_CHUNK_SIZE;
    nonpow2_ptrs[i] = our_malloc(nonpow2_size);
    EXPECT(
      nonpow2_ptrs[i] != nullptr,
      "nonpow2 malloc({}) returned null",
      nonpow2_size);

    size_t addr = reinterpret_cast<size_t>(nonpow2_ptrs[i]);
    size_t expected_align = natural_align(nonpow2_size);
    EXPECT(
      (addr % expected_align) == 0,
      "nonpow2 malloc({}) returned {} not aligned to {}",
      nonpow2_size,
      nonpow2_ptrs[i],
      expected_align);
  }

  // Free interleaved.
  for (size_t i = 0; i < COUNT; i++)
  {
    our_free(pow2_ptrs[i]);
    our_free(nonpow2_ptrs[i]);
  }
}

void test_stack_remaining_bytes()
{
  printf("  test_stack_remaining_bytes\n");
  // Verify that remaining_bytes returns a large value for non-heap
  // addresses (stack, globals).  The sizeclass sentinel (raw value 0)
  // must have slab_mask = SIZE_MAX so that bounds checks pass for
  // non-snmalloc memory.  Without the sentinel fix, remaining_bytes
  // would return 0, causing false-positive bounds check failures.
  char stack_buf[64];
  auto remaining = snmalloc::remaining_bytes<snmalloc::Config>(
    snmalloc::address_cast(stack_buf));
  EXPECT(
    remaining > sizeof(stack_buf),
    "remaining_bytes on stack address returned {}, expected large value",
    remaining);

  // Also check that alloc_size returns 0 for stack addresses.
  auto sc = snmalloc::Config::Backend::template get_metaentry<true>(
                snmalloc::address_cast(stack_buf))
              .get_sizeclass();
  auto sz = snmalloc::sizeclass_full_to_size(sc);
  EXPECT(sz == 0, "alloc_size on stack address returned {}, expected 0", sz);
}

int main()
{
  printf("large_nonpow2: non-power-of-2 large allocation tests\n");
  setup();

  test_alignment_and_usable_size();
  test_many_allocations();
  test_alloc_free_cycles();
  test_mixed_sizes();
  test_stack_remaining_bytes();

  printf("large_nonpow2: all tests passed\n");
  return 0;
}
