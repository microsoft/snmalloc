/**
 * Front-end counterpart to `src/test/func/large_offset/`.
 *
 * The front-end allocates non-pow2 large allocations directly:
 * `malloc(80 KiB)` reserves exactly 80 KiB (a sizeclass boundary)
 * rather than rounding up to the next power of two. This test
 * exercises the resulting per-chunk pagemap state via the public
 * recovery API (`external_pointer`, `remaining_bytes`).
 *
 * `large_offset.cc` covers the same ground at the backend boundary
 * (`Config::Backend::alloc_chunk` / `dealloc_chunk`), so the
 * per-chunk contract is gated independently of any front-end path.
 * This test gates that the front-end actually produces such
 * allocations.
 *
 * Two sets of checks:
 *
 *   1. Pure table-level round-tripping over every large sizeclass:
 *      `size_to_sizeclass_full(sizeclass_full_to_size(sc)) == sc`.
 *      No allocation. Cheap and exhaustive.
 *
 *   2. End-to-end on a bounded set of representative sizeclasses
 *      (the smallest non-pow2 large class, plus a non-boundary
 *      request whose smallest enclosing class is non-pow2): allocate
 *      via the public front-end API, walk every chunk-aligned
 *      interior pointer in the logical allocation, assert
 *      `external_pointer<Start>` recovers the base and
 *      `remaining_bytes` reports the expected residual.
 */

#include "test/setup.h"

#include <iostream>
#include <snmalloc/snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

namespace
{
  bool any_failures = false;

  void fail(const char* msg)
  {
    std::cout << "FAIL: " << msg << std::endl;
    any_failures = true;
  }

  /**
   * For every representable large sizeclass `sc`, check that the
   * sizeclass encoding round-trips: a request of exactly
   * `sizeclass_full_to_size(sc)` maps back to `sc`. Failure here is
   * a pure table-encoding bug and is independent of any allocation.
   */
  void test_roundtrip_all_large()
  {
    for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
    {
      sizeclass_t sc = sizeclass_t::from_large_class(lc);
      size_t S = sizeclass_full_to_size(sc);
      sizeclass_t sc2 = size_to_sizeclass_full(S);
      if (!(sc2 == sc))
      {
        std::cout << "Round-trip fail: lc=" << lc << " S=" << S
                  << " sc.raw=" << sc.raw() << " sc2.raw=" << sc2.raw()
                  << std::endl;
        fail("round-trip");
      }
    }
  }

  /**
   * Allocate `request` via the public front-end, then walk every
   * `MIN_CHUNK_SIZE`-aligned interior address and verify pointer
   * recovery. `expected_reserve` is the reservation the allocator
   * should produce (the smallest enclosing sizeclass size).
   */
  void test_alloc_chunkwalk(size_t request, size_t expected_reserve)
  {
    void* p = snmalloc::libc::malloc(request);
    if (p == nullptr)
    {
      fail("malloc returned null");
      return;
    }

    const size_t usable = snmalloc::alloc_size(p);
    if (usable != expected_reserve)
    {
      std::cout << "alloc_size mismatch: request=" << request
                << " usable=" << usable << " expected=" << expected_reserve
                << std::endl;
      fail("alloc_size != expected reserve");
    }

    // Use the `Start` pointer recovery as the start-of-object check
    // (no `libc::is_start_of_object`): `external_pointer<Start>(p)`
    // returning `p` itself is the same property.

    for (size_t off = 0; off < usable; off += MIN_CHUNK_SIZE)
    {
      void* interior = pointer_offset(p, off);
      void* base = snmalloc::external_pointer<Start>(interior);
      if (base != p)
      {
        std::cout << "external_pointer<Start>(p + " << off << ") = " << base
                  << " expected " << p << std::endl;
        fail("external_pointer mismatch");
      }
      size_t rem = snmalloc::remaining_bytes(interior);
      if (rem != usable - off)
      {
        std::cout << "remaining_bytes(p + " << off << ") = " << rem
                  << " expected " << usable - off << std::endl;
        fail("remaining_bytes mismatch");
      }
    }

    snmalloc::libc::free(p);
  }

  /**
   * Find a non-pow2 large sizeclass to exercise. Returns the
   * sentinel `sizeclass_t{}` if none exists (e.g. INTERMEDIATE_BITS
   * == 0, all classes are pow2).
   */
  sizeclass_t find_non_pow2_large_sc()
  {
    for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
    {
      sizeclass_t sc = sizeclass_t::from_large_class(lc);
      size_t S = sizeclass_full_to_size(sc);
      if (!bits::is_pow2(S))
        return sc;
    }
    return sizeclass_t{};
  }

  void test_end_to_end()
  {
    sizeclass_t sc = find_non_pow2_large_sc();
    if (sc.raw() == 0)
    {
      std::cout
        << "No non-pow2 large sizeclass available (INTERMEDIATE_BITS == 0?); "
           "skipping end-to-end test."
        << std::endl;
      return;
    }

    const size_t S = sizeclass_full_to_size(sc);

    // Boundary request: ask for exactly the class size.
    test_alloc_chunkwalk(S, S);

    // Non-boundary request: ask for (S_prev + 1) to land at S via
    // the ceil encoding. S_prev is the previous class's size; if sc
    // is the very first large class, fall back to MAX_SMALL+1.
    size_t S_prev;
    if (sc.as_large() == 0)
    {
      S_prev = MAX_SMALL_SIZECLASS_SIZE;
    }
    else
    {
      S_prev = sizeclass_full_to_size(
        sizeclass_t::from_large_class(sc.as_large() - 1));
    }
    if (S_prev + 1 < S)
    {
      test_alloc_chunkwalk(S_prev + 1, S);
    }
  }
} // namespace

int main()
{
  setup();
  test_roundtrip_all_large();
  test_end_to_end();
  if (any_failures)
  {
    std::cout << "FAILED" << std::endl;
    return 1;
  }
  std::cout << "PASSED" << std::endl;
  return 0;
}
