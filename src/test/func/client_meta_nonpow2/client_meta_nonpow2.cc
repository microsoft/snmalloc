/**
 * Exercises the slab metadata allocation path with a ClientMetaDataProvider
 * whose per-slab extra_bytes is non-power-of-two.
 *
 * The backend rounds slab metadata sizes to `MIN_META_ALIGN` (= the meta
 * range's UNIT_SIZE) rather than the next power of two, so a non-pow2
 * client meta size actually occupies a non-pow2 slab metadata block.
 * This test gates the alloc/dealloc round-trip on that path: if
 * `meta_size_round` is wrong, an inconsistent alloc/dealloc size would
 * either trip an assertion in the meta range or leak.
 */

#include "test/setup.h"

#include <iostream>
#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/snmalloc_core.h>
#include <vector>

namespace snmalloc
{
  /**
   * Per-slab client meta: `max_count + 7` bytes of storage. With
   * `StorageType = uint8_t`, the resulting extra_bytes
   * (= (required_count - 1) * 1) is non-power-of-two for typical
   * sizeclass slab object counts.
   */
  struct NonPow2ClientMetaDataProvider
  {
    using StorageType = uint8_t;
    using DataRef = uint8_t&;

    static size_t required_count(size_t max_count)
    {
      return max_count + 7;
    }

    static DataRef get(StorageType* base, size_t index)
    {
      return base[index];
    }
  };

  using Config =
    snmalloc::StandardConfigClientMeta<NonPow2ClientMetaDataProvider>;
} // namespace snmalloc

#define SNMALLOC_PROVIDE_OWN_CONFIG
#include <snmalloc/snmalloc.h>

int main()
{
#if defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  // This test does not make sense in GWP-ASan mode.
  return 0;
#else
  // Spread allocations across several small sizeclasses to force a
  // variety of slab metadata sizes; each combination of (slab object
  // count, +7 bytes) produces a different non-pow2 extra_bytes.
  constexpr size_t sizes[] = {16, 48, 96, 192, 512, 1024};
  std::vector<std::pair<void*, uint8_t>> ptrs;

  for (size_t round = 0; round < 5; round++)
  {
    for (size_t s : sizes)
    {
      for (size_t i = 0; i < 200; i++)
      {
        auto p = snmalloc::libc::malloc(s);
        auto& meta = snmalloc::get_client_meta_data(p);
        uint8_t tag = static_cast<uint8_t>((round * 31 + s + i) & 0xff);
        meta = tag;
        memset(p, tag, s);
        ptrs.emplace_back(p, tag);
      }
    }
  }

  for (auto [p, tag] : ptrs)
  {
    auto& meta = snmalloc::get_client_meta_data(p);
    if (meta != tag)
    {
      std::cout << "Meta mismatch: expected " << int(tag) << " got "
                << int(meta) << std::endl;
      abort();
    }
    snmalloc::libc::free(p);
  }

  return 0;
#endif
}
