/**
 * Unit test for LazyArrayClientMetaDataProvider (Phase 2.0).
 *
 * Validates the structural invariants of the lazy-allocated per-slab
 * client-metadata provider:
 *
 *   1. StorageType is exactly one pointer of overhead (sizeof(void*)),
 *      regardless of T or the per-slab object count.
 *   2. required_count(N) is 1 for every N — one pagemap slot per slab.
 *   3. StorageType is default-constructible and zero-initialises the
 *      backing pointer to null (matches the placement-new contract in
 *      mem/metadata.h and the null_meta_store fallback in
 *      global/globalalloc.h).
 *   4. The backing array is NOT materialised until the first get() call.
 *   5. After the first get() the backing pointer is stable: repeated
 *      get() calls return references into the same array.
 *
 * No allocator/frontend interaction: the provider is exercised against
 * a stack-resident StorageType, and the lazy install path goes
 * straight to the PAL.  The test is mitigation-independent.
 */

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <snmalloc/snmalloc_core.h>
#include <snmalloc/backend_helpers/commonconfig.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>

using snmalloc::LazyArrayClientMetaDataProvider;

namespace
{
  // A representative profiling-style payload.  Using a non-pointer T
  // guards against the storage being accidentally specialised to T*.
  using Provider = LazyArrayClientMetaDataProvider<uint64_t>;
  using Storage = Provider::StorageType;

  // --- Compile-time invariants -------------------------------------------

  // Phase 2.0: exactly one pointer of inline overhead per slab.
  static_assert(
    sizeof(Storage) == sizeof(void*),
    "LazyArrayClientMetaDataProvider::StorageType must be exactly one "
    "pointer wide");

  // The storage type must align as a pointer so it can live inline at
  // the tail of FrontendSlabMetadata with no extra padding.
  static_assert(
    alignof(Storage) == alignof(void*),
    "LazyArrayClientMetaDataProvider::StorageType must align as a pointer");

  // required_count is the same constant regardless of the caller-supplied
  // upper bound: the provider only needs one pagemap slot per slab.
  static_assert(
    Provider::required_count(1) == 1,
    "required_count must be 1 for any max_count");
  static_assert(
    Provider::required_count(64) == 1,
    "required_count must be 1 for any max_count");
  static_assert(
    Provider::required_count(SIZE_MAX) == 1,
    "required_count must be 1 for any max_count");

  // StorageType is default-constructible (and constructible by placement
  // new with no argument) — required by FrontendSlabMetadata::initialise
  // and the null_meta_store fallback.
  static_assert(
    std::is_default_constructible_v<Storage>,
    "LazyArrayClientMetaDataProvider::StorageType must be default "
    "constructible");
}

static void test_zero_initialised()
{
  Storage s{};
  if (s.backing.load(std::memory_order_relaxed) != nullptr)
  {
    std::cout << "Failed: default-constructed StorageType is not "
                 "zero-initialised (backing pointer non-null)"
              << std::endl;
    abort();
  }
}

static void test_no_allocation_before_first_get()
{
  Storage s{};
  // No call to get() yet: backing array must still be unallocated.
  if (s.backing.load(std::memory_order_relaxed) != nullptr)
  {
    std::cout << "Failed: backing array allocated before first get()"
              << std::endl;
    abort();
  }
}

static void test_get_allocates_and_is_stable()
{
  // A modest per-slab object count; the actual backing buffer will be
  // page-rounded by the PAL, so even small counts test the full path.
  constexpr size_t slab_object_count = 16;

  Storage s{};

  // First get(): triggers PAL-backed install of the backing array.
  auto& r0 = Provider::get(&s, /*index=*/3, slab_object_count);

  auto* backing_after = s.backing.load(std::memory_order_relaxed);
  if (backing_after == nullptr)
  {
    std::cout << "Failed: backing pointer still null after first get()"
              << std::endl;
    abort();
  }

  // Repeated get() at the same index must return a reference to the
  // same slot, not a re-allocation.
  auto& r1 = Provider::get(&s, /*index=*/3, slab_object_count);
  if (&r0 != &r1)
  {
    std::cout << "Failed: repeated get(idx=3) returned a different "
                 "reference (backing array not stable)"
              << std::endl;
    abort();
  }

  // A neighbouring index must fall inside the same lazily-allocated
  // array: addresses should be co-located within
  // [backing, backing + slab_object_count).
  auto& r_neighbour = Provider::get(&s, /*index=*/4, slab_object_count);
  auto* base = backing_after;
  auto* end = base + slab_object_count;
  auto* p_r0 = &r0;
  auto* p_rn = &r_neighbour;
  if (p_r0 < base || p_r0 >= end || p_rn < base || p_rn >= end)
  {
    std::cout << "Failed: get() returned a reference outside the "
                 "lazily-allocated backing array"
              << std::endl;
    abort();
  }

  // The backing pointer must not drift across get() calls.
  if (s.backing.load(std::memory_order_relaxed) != backing_after)
  {
    std::cout << "Failed: backing pointer changed across get() calls"
              << std::endl;
    abort();
  }

  // Zero-initialisation contract: PAL::notify_using<YesZero> guarantees
  // the backing buffer is observably zero on first read.
  if (r0 != 0 || r_neighbour != 0)
  {
    std::cout << "Failed: lazily-allocated backing array is not "
                 "zero-initialised on first read"
              << std::endl;
    abort();
  }

  // Round-trip a write: confirms the storage is readable and writable
  // through the returned reference.
  r0 = 0xfeedfaceULL;
  auto& r0_again = Provider::get(&s, /*index=*/3, slab_object_count);
  if (r0_again != 0xfeedfaceULL)
  {
    std::cout << "Failed: write through DataRef not visible on subsequent "
                 "get() at the same index"
              << std::endl;
    abort();
  }
}

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);

  setup();

  test_zero_initialised();
  test_no_allocation_before_first_get();
  test_get_allocates_and_is_stable();

  return 0;
}
