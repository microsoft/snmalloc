/**
 * Regression test for the compile-time aligned alloc/dealloc API.
 *
 * `snmalloc::alloc<size, Conts, align>()` applies
 * `aligned_size(align, size)` internally so the underlying reservation
 * is large enough to satisfy `align`. The matching
 * `snmalloc::dealloc<size, align>(p)` overload mirrors that: it applies
 * the same `aligned_size` before `check_size`, so the size fed to the
 * sized-dealloc sanity check is the size that was actually reserved.
 *
 * Without the aligned dealloc overload, callers either had to use the
 * unsized `dealloc(p)` or manually pass `dealloc<aligned_size(align,
 * size)>(p)`. Calling `dealloc<size>(p)` instead trips `check_size`
 * under `mitigations(sanity_checks)` whenever the alignment upgrade
 * pushes the reservation into a different sizeclass than `size`
 * itself (e.g. `S = 33 KiB`, `A = 128 KiB`: the reservation lives in
 * a 128 KiB sizeclass but `check_size` evaluates
 * `size_to_sizeclass_full(33 KiB)`, a smaller class).
 */

#include "test/setup.h"
#include "test/snmalloc_testlib.h"

#include <iostream>

using namespace snmalloc;

namespace
{
  bool any_failures = false;

  void fail(const char* msg)
  {
    std::cout << "FAIL: " << msg << std::endl;
    any_failures = true;
  }

  template<size_t size, size_t align>
  void check_round_trip(const char* label)
  {
    void* p = snmalloc::alloc<size, ZeroMem::NoZero, align>();
    if (p == nullptr)
    {
      fail(label);
      return;
    }
    constexpr size_t reserved = aligned_size(align, size);
    if (alloc_size(p) < reserved)
    {
      std::cout << "  reservation too small: alloc_size=" << alloc_size(p)
                << " expected>=" << reserved << std::endl;
      fail(label);
      return;
    }
    snmalloc::dealloc<size, align>(p);
  }
} // namespace

int main(int, char**)
{
  setup();

  // The canonical pre-existing reproducer: today's pow2 rounding maps
  // 33 KiB to one large sizeclass while the alignment-driven
  // reservation lands in a strictly larger one.
  check_round_trip<33 * 1024, 128 * 1024>("S=33KiB A=128KiB");

  // Small-to-large alignment upgrade.
  check_round_trip<48, 64 * 1024>("S=48B A=64KiB");

  // Wider gap between requested size and required alignment.
  check_round_trip<17 * 1024, 256 * 1024>("S=17KiB A=256KiB");

  // align == size: alloc and dealloc sees the same value pre- and
  // post-aligned_size; serves as a baseline that the overload
  // doesn't pessimise the simple case.
  check_round_trip<64 * 1024, 64 * 1024>("S=64KiB A=64KiB");

  // Small allocation, natural alignment.
  check_round_trip<32, 32>("S=32B A=32B");

  if (any_failures)
  {
    std::cout << "aligned_dealloc test FAILED" << std::endl;
    return 1;
  }

  std::cout << "aligned_dealloc test passed" << std::endl;
  return 0;
}
