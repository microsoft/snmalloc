#include "test/setup.h"

#include <iostream>
#include <snmalloc/backend/globalconfig.h>
#include <snmalloc/snmalloc_core.h>

namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::StandardConfigClientMeta<
    ArrayClientMetaDataProvider<std::atomic<size_t>>>>;
}
#define SNMALLOC_PROVIDE_OWN_CONFIG
#include <snmalloc/snmalloc.h>

int main()
{
#ifdef SNMALLOC_PASS_THROUGH
  // This test does not make sense in pass-through
  return 0;
#else
  std::vector<void*> ptrs;
  for (size_t i = 0; i < 10000; i++)
  {
    auto p = snmalloc::libc::malloc(1024);
    auto& meta = snmalloc::libc::get_client_meta_data(p);
    meta = i;
    ptrs.push_back(p);
    memset(p, (uint8_t)i, 1024);
  }

  for (size_t i = 0; i < 10000; i++)
  {
    auto p = ptrs[i];
    auto& meta = snmalloc::libc::get_client_meta_data(p);
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

  auto& meta = snmalloc::libc::get_client_meta_data_const(&ptrs);
  std::cout << "meta for stack" << meta << std::endl;
  return 0;
#endif
}
