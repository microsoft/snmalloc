#include "test/setup.h"

#include <iostream>
#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

using CustomGlobals = FixedRangeConfig<DefaultPal>;
using FixedAlloc = Allocator<CustomGlobals>;

// This is a variation on the fixed_alloc test
// The only difference is that we use the normal PAL here
// and make sure we actually perform the right commit calls
int main()
{
  setup();

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  auto size = bits::one_at_bit(28);
  auto oe_base = DefaultPal::reserve(size);
  auto oe_end = pointer_offset(oe_base, size);
  std::cout << "Allocated region " << oe_base << " - "
            << pointer_offset(oe_base, size) << std::endl;

  CustomGlobals::init(nullptr, oe_base, size);
  auto a = get_scoped_allocator<FixedAlloc>();

  size_t object_size = 128;
  size_t count = 0;
  size_t i = 0;
  while (true)
  {
    auto r1 = a->alloc(object_size);

    count += object_size;
    i++;

    // Run until we exhaust the fixed region.
    // This should return null.
    if (r1 == nullptr)
      break;

    if (!snmalloc::is_owned<CustomGlobals>(r1))
    {
      a->dealloc(r1);
      continue;
    }

    if (i == 1024)
    {
      i = 0;
      std::cout << ".";
    }

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
}
