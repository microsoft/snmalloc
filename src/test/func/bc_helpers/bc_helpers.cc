/**
 * Unit tests for BitmapCoalesceHelpers — the pure arithmetic layer.
 *
 * Tests bin_index mapping, allocation lookup (start bit, mask bit),
 * threshold monotonicity, decompose round-trips, and exhaustive
 * verification against brute-force servable-set computation.
 */

#include <iostream>
#include <vector>

#include <snmalloc/snmalloc_core.h>
#include <test/setup.h>

using namespace snmalloc;

// Use a small MAX_SIZE_BITS for testing so the bitmap is manageable.
// 22 gives MAX_EXPONENT = 22 - MIN_CHUNK_BITS, enough for several exponent
// levels while keeping exhaustive tests fast.
// But for the helpers tests we can also just use the full address space.
// We'll use bits::BITS - 1 to match the real configuration.
using BC = BitmapCoalesceHelpers<bits::BITS - 1>;

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

// ---- Brute-force servable-set computation ----

/**
 * Can a block at `addr` (in chunks) of `block_size` (in chunks)
 * serve size class `sc` (in chunks) with natural alignment?
 */
static bool can_serve(size_t addr, size_t block_size, size_t sc)
{
  size_t align = BC::natural_alignment(sc);
  // First aligned address >= addr
  size_t aligned_addr = ((addr + align - 1) / align) * align;
  return aligned_addr + sc <= addr + block_size;
}

/**
 * Generate all valid size classes up to max_size (in chunks).
 */
static std::vector<size_t> gen_size_classes(size_t max_size)
{
  std::vector<size_t> classes;
  classes.push_back(1);
  if (max_size >= 2)
    classes.push_back(2);
  if (max_size >= 3)
    classes.push_back(3);

  size_t e = 2;
  while (true)
  {
    size_t base = size_t(1) << e;
    if (base > max_size)
      break;
    size_t step = base >> INTERMEDIATE_BITS;
    for (size_t m = 0; m < BC::SL_COUNT; m++)
    {
      size_t s = base + m * step;
      if (s > max_size)
        break;
      classes.push_back(s);
    }
    e++;
  }
  return classes;
}

// ---- Test: constants sanity ----

void test_constants()
{
  std::cout << "test_constants..." << std::endl;

  CHECK(BC::B == 2);
  CHECK(BC::SL_COUNT == 4);
  CHECK(BC::SLOTS_PER_EXPONENT == 5);
  CHECK(BC::PREFIX_BITS == 3);

  // For full address space: MAX_EXPONENT should be BITS-1 - MIN_CHUNK_BITS
  CHECK(BC::MAX_EXPONENT == bits::BITS - 1 - MIN_CHUNK_BITS);

  // NUM_BINS = 3 + 5 * (MAX_EXPONENT - 1)
  CHECK(BC::NUM_BINS == 3 + 5 * (BC::MAX_EXPONENT - 1));

  // BITMAP_WORDS should be enough to hold NUM_BINS bits
  CHECK(BC::BITMAP_WORDS * bits::BITS >= BC::NUM_BINS);
  CHECK((BC::BITMAP_WORDS - 1) * bits::BITS < BC::NUM_BINS);
}

// ---- Test: decompose / sizeclass_size round-trip ----

void test_decompose_roundtrip()
{
  std::cout << "test_decompose_roundtrip..." << std::endl;

  // All valid size classes up to 1024 chunks
  auto classes = gen_size_classes(1024);

  for (size_t sc : classes)
  {
    size_t e, m;
    bool ok = BC::decompose(sc, e, m);
    CHECK(ok);
    if (ok)
    {
      CHECK(BC::sizeclass_size(e, m) == sc);
    }
  }

  // Invalid sizes should not decompose
  CHECK(!BC::decompose(0, *reinterpret_cast<size_t*>(&failure_count),
                        *reinterpret_cast<size_t*>(&failure_count)));
  // 9 is not a valid size class (between 8 and 10 for B=2)
  size_t e_tmp, m_tmp;
  CHECK(!BC::decompose(9, e_tmp, m_tmp));
  CHECK(!BC::decompose(11, e_tmp, m_tmp));
  CHECK(!BC::decompose(13, e_tmp, m_tmp));
  CHECK(!BC::decompose(15, e_tmp, m_tmp));
}

// ---- Test: is_valid_sizeclass ----

void test_is_valid_sizeclass()
{
  std::cout << "test_is_valid_sizeclass..." << std::endl;

  auto classes = gen_size_classes(256);

  for (size_t s = 0; s <= 256; s++)
  {
    bool expected = false;
    for (size_t sc : classes)
    {
      if (sc == s)
      {
        expected = true;
        break;
      }
    }
    if (BC::is_valid_sizeclass(s) != expected)
    {
      std::cout << "  is_valid_sizeclass(" << s << ") = "
                << BC::is_valid_sizeclass(s)
                << ", expected " << expected << std::endl;
      CHECK(false);
    }
  }
}

// ---- Test: alloc_start_bit / alloc_mask_bit ----

void test_alloc_lookup()
{
  std::cout << "test_alloc_lookup..." << std::endl;

  auto classes = gen_size_classes(1024);

  for (size_t sc : classes)
  {
    size_t e, m;
    bool ok = BC::decompose(sc, e, m);
    CHECK(ok);
    if (!ok)
      continue;

    size_t start = BC::alloc_start_bit(e, m);
    CHECK(start < BC::NUM_BINS);

    if (e >= 2 && m == 0)
    {
      size_t mask = BC::alloc_mask_bit(e);
      CHECK(mask < BC::NUM_BINS);
      CHECK(mask != start); // mask bit should be different from start
    }
    else if (e >= 2)
    {
      // m != 0: alloc_mask_bit for e should still be valid
      // but we don't use it; the search is unmasked.
    }
    if (e <= 1)
    {
      // Prefix range: no masking
      CHECK(BC::alloc_mask_bit(e) == SIZE_MAX);
    }
  }
}

// ---- Test: threshold monotonicity ----

void test_threshold_monotonicity()
{
  std::cout << "test_threshold_monotonicity..." << std::endl;

  // For a fixed alignment, bin_index(n, alpha) should be non-decreasing in n.
  for (size_t alpha_bits = 0; alpha_bits <= 8; alpha_bits++)
  {
    size_t alpha = size_t(1) << alpha_bits;
    size_t prev_bin = 0;
    for (size_t n = 1; n <= 256; n++)
    {
      size_t bin = BC::bin_index(n, alpha);
      if (bin < prev_bin)
      {
        std::cout << "  MONOTONICITY FAIL: bin_index(" << n << ", " << alpha
                  << ") = " << bin << " < prev " << prev_bin << std::endl;
        CHECK(false);
      }
      prev_bin = bin;
    }
  }
}

// ---- Test: exhaustive bin_index verification ----
// For every (addr, block_size) in a small arena, compute the servable set
// by brute force, then verify bin_index assigns a bin whose servable set
// is a subset of the block's actual servable set.

void test_exhaustive_bin_index()
{
  std::cout << "test_exhaustive_bin_index..." << std::endl;

  constexpr size_t ARENA = 256;
  auto classes = gen_size_classes(ARENA);

  // For each bin index, compute its servable set (conservatively).
  // A bin's servable set = intersection of servable sets of all blocks in it.
  // But we verify from the other direction: for each block, verify that
  // every size class claimable from the block's bin is actually servable.

  // First, build the mapping: for each (e, m), which bins have start_bit
  // and what's the search path?
  //
  // The key property to verify: if bin_index assigns a block to bin B, and
  // an allocation for sizeclass S would find bin B (i.e., B >= start_bit
  // and B is not masked), then the block MUST be able to serve S.

  size_t errors = 0;

  for (size_t addr = 0; addr < ARENA; addr++)
  {
    for (size_t n = 1; n + addr <= ARENA; n++)
    {
      // We want alignment in chunk units; since these ARE chunk units already,
      // alpha_chunks = natural_alignment(addr) for addr > 0, or large for 0.
      size_t alpha_chunks;
      if (addr == 0)
        alpha_chunks = size_t(1) << (bits::BITS - 1);
      else
        alpha_chunks = BC::natural_alignment(addr);

      size_t bin = BC::bin_index(n, alpha_chunks);
      CHECK(bin < BC::NUM_BINS);

      // For every size class S that would find this bin during allocation:
      // The search starts at alloc_start_bit(e, m) and walks upward,
      // optionally masking one bit.  If bin >= start_bit and bin is not
      // masked, the allocation would pick this block.
      for (size_t sc : classes)
      {
        size_t e, m;
        if (!BC::decompose(sc, e, m))
          continue;

        size_t start = BC::alloc_start_bit(e, m);

        // Would this bin be found by the search?
        if (bin < start)
          continue; // bin is below the search window

        // Check if bin is the masked-out bit
        if (m == 0 && e >= 2)
        {
          size_t mask = BC::alloc_mask_bit(e);
          if (bin == mask)
            continue; // this bit is masked out for m=0
        }

        // The allocation would pick this block.  Verify it can serve S.
        if (!can_serve(addr, n, sc))
        {
          errors++;
          if (errors <= 10)
          {
            std::cout << "  bin_index ERROR: addr=" << addr << " n=" << n
                      << " alpha=" << alpha_chunks << " -> bin=" << bin
                      << ", but can't serve sc=" << sc << " (e=" << e
                      << " m=" << m << " start=" << start << ")" << std::endl;
          }
        }
      }
    }
  }

  if (errors > 0)
  {
    std::cout << "  " << errors << " errors in exhaustive bin_index check"
              << std::endl;
  }
  CHECK(errors == 0);
}

// ---- Test: threshold-based completeness ----
// If a block meets the threshold for size class S (i.e. n >= T(S, alpha)),
// then the allocation search for S must be able to find the block's bin.
// Note: this is weaker than "can_serve implies findable" because the
// threshold is a worst-case bound over all addresses with a given alignment.
// Some blocks at favorable addresses can serve S even when n < T(S, alpha),
// but those are not guaranteed findable.

void test_threshold_completeness()
{
  std::cout << "test_threshold_completeness..." << std::endl;

  constexpr size_t ARENA = 64;
  auto classes = gen_size_classes(ARENA);

  size_t errors = 0;

  for (size_t addr = 0; addr < ARENA; addr++)
  {
    for (size_t n = 1; n + addr <= ARENA; n++)
    {
      size_t alpha_chunks;
      if (addr == 0)
        alpha_chunks = size_t(1) << (bits::BITS - 1);
      else
        alpha_chunks = BC::natural_alignment(addr);

      size_t bin = BC::bin_index(n, alpha_chunks);

      for (size_t sc : classes)
      {
        // Only check classes where the threshold is met.
        size_t t = BC::threshold(sc, alpha_chunks);
        if (n < t)
          continue;

        size_t e, m;
        if (!BC::decompose(sc, e, m))
          continue;

        size_t start = BC::alloc_start_bit(e, m);

        // bin should be >= start (the block should be findable)
        if (bin < start)
        {
          errors++;
          if (errors <= 10)
          {
            std::cout << "  COMPLETENESS: addr=" << addr << " n=" << n
                      << " alpha=" << alpha_chunks << " -> bin=" << bin
                      << ", but sc=" << sc << " (e=" << e << " m=" << m
                      << " start=" << start << " T=" << t << ") not findable"
                      << std::endl;
          }
        }

        // bin should not be the masked bit (if m=0)
        if (m == 0 && e >= 2)
        {
          size_t mask = BC::alloc_mask_bit(e);
          if (bin == mask)
          {
            errors++;
            if (errors <= 10)
            {
              std::cout << "  COMPLETENESS: addr=" << addr << " n=" << n
                        << " alpha=" << alpha_chunks << " -> bin=" << bin
                        << " == mask for sc=" << sc << " (e=" << e << ")"
                        << std::endl;
            }
          }
        }
      }
    }
  }

  if (errors > 0)
  {
    std::cout << "  " << errors << " threshold completeness errors"
              << std::endl;
  }
  CHECK(errors == 0);
}

// ---- Main ----

int main()
{
  setup();

  test_constants();
  test_decompose_roundtrip();
  test_is_valid_sizeclass();
  test_alloc_lookup();
  test_threshold_monotonicity();
  test_exhaustive_bin_index();
  test_threshold_completeness();

  if (failure_count > 0)
  {
    std::cout << "\n" << failure_count << " FAILURES" << std::endl;
    return 1;
  }

  std::cout << "\nAll bc_helpers tests passed." << std::endl;
  return 0;
}
