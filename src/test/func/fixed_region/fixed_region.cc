
#define OPEN_ENCLAVE
#define OPEN_ENCLAVE_SIMULATION
#define USE_RESERVE_MULTIPLE 1
#include <iostream>
#include <snmalloc.h>

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

using namespace snmalloc;
int main()
{
  MemoryProviderStateMixin<DefaultPal> mp;

  size_t size = 1ULL << 28;
  oe_base = mp.reserve<true>(&size, 0);
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
