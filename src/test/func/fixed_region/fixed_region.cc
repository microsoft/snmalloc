#define SNMALLOC_SGX
#define OPEN_ENCLAVE
#define OPEN_ENCLAVE_SIMULATION
#include <iostream>
#include <snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size)
{
  UNUSED(p_size);
  return memset(p, c, size);
}

extern "C" void oe_abort()
{
  abort();
}

using namespace snmalloc;
int main()
{
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  auto& mp = *MemoryProviderStateMixin<DefaultPal>::make();

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  size_t large_class = 28 - SUPERSLAB_BITS;
  size_t size = 1ULL << (SUPERSLAB_BITS + large_class);
  void* oe_base = mp.reserve<true>(large_class);
  void* oe_end = (uint8_t*)oe_base + size;
  PALOpenEnclave::setup_initial_range(oe_base, oe_end);
  std::cout << "Allocated region " << oe_base << " - " << oe_end << std::endl;

  auto a = ThreadAlloc::get();

  while (true)
  {
    auto r1 = a->alloc(100);

    // Run until we exhaust the fixed region.
    // This should return null.
    if (r1 == nullptr)
      return 0;

    if (oe_base > r1)
      abort();
    if (oe_end < r1)
      abort();
  }
#endif
}
