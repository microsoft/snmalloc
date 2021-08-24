#include "backend/fixedglobalconfig.h"
#include "test/setup.h"

#include <iostream>
#include <snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

using CustomGlobals = FixedGlobals<PALNoAlloc<DefaultPal>>;
using FixedAlloc = LocalAllocator<CustomGlobals>;

int main()
{
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  setup();

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  auto size = bits::one_at_bit(28);
  auto oe_base = Pal::reserve(size);
  Pal::notify_using<NoZero>(oe_base, size);
  auto oe_end = pointer_offset(oe_base, size);
  std::cout << "Allocated region " << oe_base << " - "
            << pointer_offset(oe_base, size) << std::endl;

  CustomGlobals::init(nullptr, oe_base, size);
  FixedAlloc a;

  size_t object_size = 128;
  size_t count = 0;
  size_t i = 0;
  while (true)
  {
    auto r1 = a.alloc(object_size);
    count += object_size;
    i++;

    if (i == 1024)
    {
      i = 0;
      std::cout << ".";
    }
    // Run until we exhaust the fixed region.
    // This should return null.
    if (r1 == nullptr)
      break;

    if (oe_base > r1)
    {
      std::cout << "Allocated: " << r1 << std::endl;
      abort();
    }
    if (oe_end < r1)
    {
      std::cout << "Allocated: " << r1 << std::endl;
      abort();
    }
  }

  std::cout << "Total allocated: " << count << " out of " << size << std::endl;
  std::cout << "Overhead: 1/" << (double)size / (double)(size - count)
            << std::endl;

  a.teardown();
#endif
}
