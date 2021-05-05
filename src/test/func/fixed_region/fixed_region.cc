#define SNMALLOC_SGX
#define OPEN_ENCLAVE
#define OE_OK 0
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

extern "C" int oe_random(void* p, size_t p_size)
{
  UNUSED(p_size);
  UNUSED(p);
  // Stub for random data.
  return 0;
}

extern "C" void oe_abort()
{
  abort();
}

using namespace snmalloc;
int main()
{
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  auto& mp = *ChunkAllocator<
    DefaultPal,
    DefaultArenaMap<DefaultPal, DefaultPrimAlloc>>::make();

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  size_t large_class = 28 - SUPERSLAB_BITS;
  size_t size = bits::one_at_bit(SUPERSLAB_BITS + large_class);
  void* oe_base = mp.reserve<true>(large_class).unsafe_capptr;
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
