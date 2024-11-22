#include <iostream>
#include <snmalloc/snmalloc.h>
#include <stdlib.h>
#include <string.h>
#include <test/setup.h>

extern "C" void* oe_memset_s(void* p, size_t p_size, int c, size_t size)
{
  snmalloc::UNUSED(p_size);
  return memset(p, c, size);
}

extern "C" int oe_random(void* p, size_t p_size)
{
  snmalloc::UNUSED(p_size, p);
  // Stub for random data.
  return 0;
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

using namespace snmalloc;

int main()
{
  setup();

  // 26 is large enough to produce a nested allocator.
  // many other sizes would work.
  size_t length = bits::one_at_bit(26);
  auto oe_base = host_malloc(length);

  auto oe_end = pointer_offset(oe_base, length);
  oe_allocator_init(oe_base, oe_end);

  std::cout << "Allocated region " << oe_base << " - " << oe_end << std::endl;

  auto a = host_malloc(128);
  auto b = enclave_malloc(128);

  std::cout << "Host alloc " << a << std::endl;
  std::cout << "Enclave alloc " << b << std::endl;

  host_free(a);
  std::cout << "Host freed!" << std::endl;
  enclave_free(b);
  std::cout << "Enclace freed!" << std::endl;
}
