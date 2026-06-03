/**
 * Targeted test for the per-chunk pagemap offset write path in
 * `BackendAllocator::alloc_chunk`.
 *
 * The front end currently only issues pow2 large requests (the
 * `slab_size >= size` fast path), so the multi-slab-tile branch in
 * `alloc_chunk` writing per-chunk offsets is otherwise unreachable
 * from the in-tree allocation paths. This test reaches it via the
 * public backend API.
 *
 * Method:
 *   - Pick a non-pow2 large sizeclass `sc` whose
 *     `sizeclass_full_to_slab_size(sc) < sizeclass_full_to_size(sc)`,
 *     so the multi-slab-tile branch triggers.
 *   - Compute the pow2 reservation `next_pow2(size)` (the size
 *     `alloc_chunk` asserts).
 *   - Call `Config::Backend::alloc_chunk` directly with that pow2 size
 *     and the non-pow2 sc.
 *   - For each chunk in the pow2 region verify the pagemap entry's
 *     `get_offset_and_sizeclass()` decomposes into the expected
 *     (sc, slab_index) pair.
 *   - For sampled interior addresses verify that
 *     `remaining_bytes` / `index_in_object` return positions within
 *     the logical allocation.
 *   - Verify `is_start_of_object` behaviour: true at the allocation
 *     base, false elsewhere.
 *   - `dealloc_chunk` and verify entries clear back to "not
 *     frontend-owned" (low COMBINED_BITS == 0).
 */

#include "test/setup.h"

#include <iostream>
#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

using CustomGlobals = FixedRangeConfig<PALNoAlloc<DefaultPal>>;
using FixedAlloc = Allocator<CustomGlobals>;

namespace
{
  bool any_failures = false;

  void fail(const char* msg)
  {
    std::cout << "FAIL: " << msg << std::endl;
    any_failures = true;
  }

  /**
   * Find the smallest non-pow2 large sizeclass: one where slab_size <
   * size. Returns sizeclass_t{} (the unmapped sentinel) if none exists
   * in this configuration.
   */
  sizeclass_t find_non_pow2_large_sc()
  {
    for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
    {
      auto sc = sizeclass_t::from_large_class(lc);
      const size_t size = sizeclass_full_to_size(sc);
      const size_t slab_size = sizeclass_full_to_slab_size(sc);
      if (slab_size < size)
        return sc;
    }
    return sizeclass_t{};
  }

  void test_per_chunk_offset()
  {
    auto sc = find_non_pow2_large_sc();
    if (sc.raw() == 0)
    {
      std::cout << "No non-pow2 large sizeclass available in this config; "
                   "skipping per-chunk offset test."
                << std::endl;
      return;
    }
    const size_t size = sizeclass_full_to_size(sc);
    const size_t slab_size = sizeclass_full_to_slab_size(sc);
    const size_t reserve = bits::next_pow2(size);

    std::cout << "non-pow2 sc raw=" << sc.raw() << " size=" << size
              << " slab_size=" << slab_size << " reserve=" << reserve
              << std::endl;

    // Set up an isolated FixedRangeConfig allocator. FixedRangeConfig
    // owns its own pagemap and never reclaims `region_base`; the
    // reservation is released when the process exits. For a multi-
    // test harness, explicit teardown would be required here.
    const size_t region = bits::one_at_bit(28);
    auto region_base = DefaultPal::reserve(region);
    DefaultPal::notify_using<NoZero>(region_base, region);
    CustomGlobals::init(nullptr, region_base, region);

    auto a = get_scoped_allocator<FixedAlloc>();

    using Backend = typename CustomGlobals::Backend;
    using Entry = typename CustomGlobals::PagemapEntry;

    // Construct the encoded ras the way the front end does (offset=0).
    const uintptr_t ras_in = Entry::encode(nullptr, sc);

    auto [chunk, slab_meta] =
      Backend::alloc_chunk(a->get_backend_local_state(), reserve, ras_in, sc);
    if (chunk == nullptr)
    {
      fail("alloc_chunk returned null");
      return;
    }

    const address_t base = address_cast(chunk);
    std::cout << "Allocated chunk base=" << reinterpret_cast<void*>(base)
              << " reserve=" << reserve << std::endl;

    // Verify per-chunk pagemap entries.
    for (size_t chunk_offset = 0; chunk_offset < reserve;
         chunk_offset += MIN_CHUNK_SIZE)
    {
      const size_t expected_slab_index = chunk_offset / slab_size;
      const auto& entry = Backend::get_metaentry(base + chunk_offset);
      const offset_and_sizeclass_t osc = entry.get_offset_and_sizeclass();
      const offset_and_sizeclass_t expected_osc =
        offset_and_sizeclass_t(sc, expected_slab_index);
      if (!(osc == expected_osc))
      {
        std::cout << "Chunk @+" << chunk_offset << " osc=" << osc.raw()
                  << " expected=" << expected_osc.raw() << " (sc=" << sc.raw()
                  << " idx=" << expected_slab_index << ")" << std::endl;
        fail("offset_and_sizeclass mismatch");
      }
      // The pure sizeclass mask must still report `sc`.
      if (!(entry.get_sizeclass() == sc))
      {
        std::cout << "Chunk @+" << chunk_offset << " get_sizeclass mismatch"
                  << std::endl;
        fail("get_sizeclass mismatch on offset>0 chunk");
      }
    }

    // For an interior address in each chunk that lies within the
    // *logical* allocation (size, not the pow2 reservation),
    // remaining_bytes / index_in_object should report position within
    // the allocation.
    for (size_t chunk_offset = 0; chunk_offset < size;
         chunk_offset += MIN_CHUNK_SIZE)
    {
      const address_t addr = base + chunk_offset;
      const size_t rem = snmalloc::remaining_bytes<CustomGlobals>(addr);
      if (rem != size - chunk_offset)
      {
        std::cout << "remaining_bytes @+" << chunk_offset << " = " << rem
                  << " expected " << (size - chunk_offset) << std::endl;
        fail("remaining_bytes mismatch");
      }
      const size_t idx = snmalloc::index_in_object<CustomGlobals>(addr);
      if (idx != chunk_offset)
      {
        std::cout << "index_in_object @+" << chunk_offset << " = " << idx
                  << " expected " << chunk_offset << std::endl;
        fail("index_in_object mismatch");
      }
    }

    // Direct is_start_of_object checks: the allocation base address
    // must be a start-of-object; an interior address inside the first
    // slab tile (offset_bytes == 0 in pagemap) but not at the base
    // must NOT; and an address in any non-first slab tile
    // (offset_bytes != 0 in pagemap) must NOT.
    {
      const auto& base_entry = Backend::get_metaentry(base);
      if (!is_start_of_object(base_entry.get_offset_and_sizeclass(), base))
        fail("base address not reported as start-of-object");
      if (is_start_of_object(base_entry.get_offset_and_sizeclass(), base + 1))
        fail("base+1 incorrectly reported as start-of-object");
    }
    if (size > slab_size)
    {
      const address_t second_slab = base + slab_size;
      const auto& second_entry = Backend::get_metaentry(second_slab);
      if (is_start_of_object(
            second_entry.get_offset_and_sizeclass(), second_slab))
        fail("second slab tile base incorrectly reported as start-of-object");
    }

    // Tear down: dealloc the chunk and verify the per-chunk pagemap
    // entries no longer report as frontend-owned.
    auto alloc_cap =
      capptr_chunk_is_alloc(capptr_to_user_address_control(chunk));
    Backend::dealloc_chunk(
      a->get_backend_local_state(), *slab_meta, alloc_cap, reserve, sc);

    for (size_t chunk_offset = 0; chunk_offset < reserve;
         chunk_offset += MIN_CHUNK_SIZE)
    {
      const auto& entry = Backend::get_metaentry(base + chunk_offset);
      if (!entry.is_backend_owned())
      {
        std::cout << "Chunk @+" << chunk_offset
                  << " not backend-owned after dealloc; osc="
                  << entry.get_offset_and_sizeclass().raw() << std::endl;
        fail("dealloc didn't reset per-chunk offset");
      }
    }
  }
} // namespace

int main()
{
  setup();
  test_per_chunk_offset();
  if (any_failures)
  {
    std::cout << "FAILED" << std::endl;
    return 1;
  }
  std::cout << "PASSED" << std::endl;
  return 0;
}
