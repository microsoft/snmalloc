#include "test/setup.h"

#include <iostream>
#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc.h>

#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

using CustomGlobals = FixedRangeConfig<PALNoAlloc<DefaultPal>>;
using FixedAlloc = Allocator<CustomGlobals>;

/**
 * Regression test for https://github.com/microsoft/snmalloc/issues/876.
 *
 * FixedRangeConfig has a range-checking capptr_domesticate, which returns
 * nullptr for anything outside the fixed region.  BatchedRemoteMessage stores
 * a bit-packed (displacement, length) word, not a pointer, in the next field
 * of its free ring; that word used to be passed through the domesticator when
 * the receiving allocator opened the ring, so every ring decoded as length 0
 * and every batched remote deallocation was lost.
 *
 * Allocator A allocates small objects and allocator B frees them (remotely)
 * and flushes.  Everything allocated is freed, so the loop must be able to run
 * well past the point at which the fixed region would be exhausted if the
 * remote frees never made it back to A's slabs.
 */
int main()
{
  setup();

  const size_t size = bits::one_at_bit(25); // 32 MiB region
  auto base = DefaultPal::reserve(size);
  DefaultPal::notify_using<NoZero>(base, size);
  std::cout << "Allocated region " << base << " - "
            << pointer_offset(base, size) << std::endl;

  CustomGlobals::init(nullptr, base, size);

  auto a = get_scoped_allocator<FixedAlloc>();
  auto b = get_scoped_allocator<FixedAlloc>();

  constexpr size_t object_size = 64;
  constexpr size_t batch = 64;
  void* objects[batch];

  // Enough rounds to exhaust the region twice over if the remote frees were
  // being lost; the bug shows up well before the first exhaustion.
  const size_t rounds = 2 * (size / (batch * object_size));

  for (size_t round = 0; round < rounds; round++)
  {
    for (size_t i = 0; i < batch; i++)
    {
      objects[i] = a->alloc(object_size);
      if (objects[i] == nullptr)
      {
        std::cout << "Allocator A returned nullptr in round " << round
                  << " after " << round * batch
                  << " remote frees, even though everything allocated so far "
                     "has been freed."
                  << std::endl;
        abort();
      }
      SNMALLOC_CHECK(snmalloc::is_owned<CustomGlobals>(objects[i]));
    }

    // B is not the owner of these objects, so these are remote deallocations
    // that are batched into rings and pushed onto A's message queue by flush.
    for (size_t i = 0; i < batch; i++)
      b->dealloc(objects[i]);
    b->flush();

    if ((round % 1024) == 0)
      std::cout << "." << std::flush;
  }

  std::cout << std::endl
            << "Completed " << rounds << " rounds of " << batch
            << " remote frees of " << object_size << "-byte objects"
            << std::endl;
  return 0;
}
