#include "../../../snmalloc.h"

#include <iostream>
#include <stdlib.h>
#include <string.h>

void* oe_base;
void* oe_end;
extern "C" const void* __oe_get_heap_base()
{
  return oe_base;
}

extern "C" const void* __oe_get_heap_end()
{
  return oe_end;
}

extern "C" void* oe_memset(void* p, int c, size_t size)
{
  return memset(p, c, size);
}

extern "C" void oe_abort()
{
  abort();
}

extern "C" void* host_malloc(size_t);
extern "C" void host_free(void*);

extern "C" void* enclave_malloc(size_t);
extern "C" void enclave_free(void*);

extern "C" void*
enclave_snmalloc_get_global_pagemap(snmalloc::PagemapConfig const**);
extern "C" void*
host_snmalloc_get_global_pagemap(snmalloc::PagemapConfig const**);

using namespace snmalloc;
int main()
{
  MemoryProviderStateMixin<DefaultPal> mp;

  size_t size = 1ULL << 28;
  oe_base = mp.reserve<true>(&size, 1);
  oe_end = (uint8_t*)oe_base + size;
  std::cout << "Allocated region " << oe_base << " - " << oe_end << std::endl;

  // Call these functions to trigger asserts if the cast-to-self doesn't work.
  enclave_snmalloc_get_global_pagemap(nullptr);
  host_snmalloc_get_global_pagemap(nullptr);

  auto a = host_malloc(128);
  auto b = enclave_malloc(128);

  std::cout << "Host alloc " << a << std::endl;
  std::cout << "Enclave alloc " << b << std::endl;

  host_free(a);
  enclave_free(b);
}
