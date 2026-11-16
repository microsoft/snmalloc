#include "test/setup.h"

#include <chrono>
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

  {
    using Pool = AllocPool<CustomGlobals>;
    using State = AllocPoolAssistance<CustomGlobals>;

    auto count_allocators = []() {
      size_t count = 0;
      for (auto* alloc = Pool::iterate(); alloc != nullptr;
           alloc = Pool::iterate(alloc))
      {
        count++;
      }
      return count;
    };

    auto sender = get_scoped_allocator<FixedAlloc>();
    void* remote;
    {
      auto owner = get_scoped_allocator<FixedAlloc>();
      remote = owner->alloc(128);
      SNMALLOC_CHECK(remote != nullptr);
    }

    sender->dealloc(remote);
    sender->flush();
    SNMALLOC_CHECK(State::debug_pending_count() == 1);

    const size_t allocator_count = count_allocators();
    constexpr size_t batch_size = 256;
    void* batch[batch_size];
    size_t iterations = 0;
    constexpr size_t iteration_limit = 1 << 24;
    const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(
        3 * allocator_count * uint64_t{SNMALLOC_ASSIST_IDLE_MS});

    while (State::debug_pending_count() != 0)
    {
      if (
        (std::chrono::steady_clock::now() >= deadline) ||
        (iterations == iteration_limit))
      {
        std::cerr << "Fixed-range assistance did not complete: pending="
                  << State::debug_pending_count() << std::endl;
        abort();
      }

      for (auto& p : batch)
      {
        p = sender->alloc(128);
        SNMALLOC_CHECK(p != nullptr);
      }
      for (auto p : batch)
        sender->dealloc(p);
      iterations++;
    }

    SNMALLOC_CHECK(count_allocators() == allocator_count);
  }

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
