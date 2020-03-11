
#define OPEN_ENCLAVE
#define OPEN_ENCLAVE_SIMULATION
#define USE_RESERVE_MULTIPLE 1
#include <iostream>
#include <snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

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

extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size)
{
  std::cout << "Memset " << p << " (" << p_size << ") - " << size << std::endl;

  return memset(p, c, size);
}

extern "C" void oe_abort()
{
  abort();
}

using namespace snmalloc;
int main()
{
  auto& mp = *MemoryProviderStateMixin<DefaultPal>::make();

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  size_t large_class = 28 - SUPERSLAB_BITS;
  size_t size = 1ULL << (SUPERSLAB_BITS + large_class);
  oe_base = mp.reserve<true>(large_class);
  oe_end = (uint8_t*)oe_base + size;
  std::cout << "Allocated region " << oe_base << " - " << oe_end << std::endl;

  auto a = ThreadAlloc::get();

  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = a->alloc(100);
    std::cout << "Allocated object " << r1 << std::endl;

    if (oe_base > r1)
      abort();
    if (oe_end < r1)
      abort();
  }
}
