/**
 * This test performs a very simple use of the client_meta data feature in
 * snmalloc.
 */

#include "test/setup.h"

#include <atomic>
#include <iostream>
#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/snmalloc_core.h>
#include <vector>

namespace snmalloc
{
  // Create an allocator that stores an std::atomic<size_t>> per allocation.
  using Config = snmalloc::StandardConfigClientMeta<
    ArrayClientMetaDataProvider<std::atomic<size_t>>>;
}

#define SNMALLOC_PROVIDE_OWN_CONFIG
#include <snmalloc/snmalloc.h>

int main()
{
#if defined(SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION)
  // This test does not make sense in GWP-ASan mode.
  return 0;
#else
  // Allocate a bunch of objects, and store the index into the meta-data.
  std::vector<void*> ptrs;
  for (size_t i = 0; i < 10000; i++)
  {
    auto p = snmalloc::libc::malloc(1024);
    auto& meta = snmalloc::get_client_meta_data(p);
    meta = i;
    ptrs.push_back(p);
    memset(p, (uint8_t)i, 1024);
  }

  // Check meta-data contains expected value, and that the memory contains
  // the expected pattern.
  for (size_t i = 0; i < 10000; i++)
  {
    auto p = ptrs[i];
    auto& meta = snmalloc::get_client_meta_data(p);
    if (meta != i)
    {
      std::cout << "Failed at index " << i << std::endl;
      abort();
    }
    for (size_t j = 0; j < 1024; j++)
    {
      if (reinterpret_cast<uint8_t*>(p)[j] != (uint8_t)i)
      {
        std::cout << "Failed at index " << i << " byte " << j << std::endl;
        abort();
      }
    }
    snmalloc::libc::free(p);
  }

  // Access in a read-only way meta-data associated with the stack.
  // This would fail if it was accessed for write.
  auto& meta = snmalloc::get_client_meta_data_const(&ptrs);
  std::cout << "meta for stack" << meta << std::endl;

  return 0;
#endif
}
