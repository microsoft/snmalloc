#include "../../../snmalloc.h"

#include <iostream>
#include <stdlib.h>
#include <string.h>
#include <test/setup.h>

extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size)
{
  UNUSED(p_size);
  return memset(p, c, size);
}

extern "C" void oe_abort()
{
  abort();
}

extern "C" void oe_allocator_init(void* base, void* end);
extern "C" void* host_malloc(size_t);
extern "C" void host_free(void*);

extern "C" void* enclave_malloc(size_t);
extern "C" void enclave_free(void*);

extern "C" void*
enclave_snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);
extern "C" void*
host_snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);

using namespace snmalloc;
int main()
{
  setup();

  MemoryProviderStateMixin<DefaultPal> mp;

  // 26 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 2 is not big enough for the example.
  size_t large_class = 26 - SUPERSLAB_BITS;
  size_t size = 1ULL << (SUPERSLAB_BITS + large_class);
  void* oe_base = mp.reserve<true>(large_class);
  void* oe_end = (uint8_t*)oe_base + size;
  oe_allocator_init(oe_base, oe_end);
  std::cout << "Allocated region " << oe_base << " - " << oe_end << std::endl;

  // Call these functions to trigger asserts if the cast-to-self doesn't work.
  const PagemapConfig* c;
  enclave_snmalloc_pagemap_global_get(&c);
  host_snmalloc_pagemap_global_get(&c);

  auto a = host_malloc(128);
  auto b = enclave_malloc(128);

  std::cout << "Host alloc " << a << std::endl;
  std::cout << "Enclave alloc " << b << std::endl;

  host_free(a);
  enclave_free(b);
}
