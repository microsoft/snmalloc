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

void host_to_enclave_free(void* arg, void* p)
{
  // No security domain transition, but hand off to enclave allocator
  UNUSED(arg);

  std::cout << "HTEF " << p << std::endl;

  enclave_free(p);
}

extern "C" void*
enclave_snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);
extern "C" void*
host_snmalloc_pagemap_global_get(snmalloc::PagemapConfig const**);
extern "C" void*
host_snmalloc_designate_foreign(void*, size_t, struct ForeignAllocator*);

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
  //
  // Note that these check *type* compatibility but do not actually unify the
  // two Pagemaps: that is, the enclave and host allocators continue to have
  // separate pagemaps.
  const PagemapConfig* c;
  enclave_snmalloc_pagemap_global_get(&c);
  host_snmalloc_pagemap_global_get(&c);

  auto a = host_malloc(128);
  auto b = enclave_malloc(128);

  std::cout << "Host alloc " << a << std::endl;
  std::cout << "Enclave alloc " << b << std::endl;

  host_free(a);
  enclave_free(b);

  /*
   * Designate the OE space as a foreign allocator in the host allocator's maps
   */
  struct ForeignAllocator* fa_enclave =
    static_cast<ForeignAllocator*>(host_malloc(sizeof fa_enclave));
  fa_enclave->arg = NULL;
  fa_enclave->free = host_to_enclave_free;

  host_snmalloc_designate_foreign(oe_base, size, fa_enclave);

  auto x = enclave_malloc(128);
  host_free(x);
}
