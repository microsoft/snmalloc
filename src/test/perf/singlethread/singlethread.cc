#include <test/measuretime.h>
#include <test/opt.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>
#include <unordered_set>

using namespace snmalloc;

template<ZeroMem zero_mem>
void test_alloc_dealloc(size_t count, size_t size, bool write)
{
  {
    MeasureTime m;
    m << "Count: " << std::setw(6) << count << ", Size: " << std::setw(6)
      << size << ", ZeroMem: " << (zero_mem == ZeroMem::YesZero)
      << ", Write: " << write;

    std::unordered_set<void*> set;

    // alloc 1.5x objects
    for (size_t i = 0; i < ((count * 3) / 2); i++)
    {
      void* p = snmalloc::alloc<zero_mem>(size);
      SNMALLOC_CHECK(set.find(p) == set.end());

      if (write)
        *(int*)p = 4;

      set.insert(p);
    }

    // free 0.25x of the objects
    for (size_t i = 0; i < (count / 4); i++)
    {
      auto it = set.begin();
      void* p = *it;
      set.erase(it);
      SNMALLOC_CHECK(set.find(p) == set.end());
      snmalloc::dealloc(p, size);
    }

    // alloc 1x objects
    for (size_t i = 0; i < count; i++)
    {
      void* p = snmalloc::alloc<zero_mem>(size);
      SNMALLOC_CHECK(set.find(p) == set.end());

      if (write)
        *(int*)p = 4;

      set.insert(p);
    }

    // free everything
    while (!set.empty())
    {
      auto it = set.begin();
      snmalloc::dealloc(*it, size);
      set.erase(it);
    }
  }

  snmalloc::debug_check_empty();
}

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);
  // Default `count` exercises sizeclass dispatch many times; under
  // `--smoke` we keep one alloc/dealloc cycle through every code
  // path but cut the bulk repetitions.
  size_t count_small = opt.has("--smoke") ? 1u << 12 : 1u << 15;
  size_t count_large = opt.has("--smoke") ? 1u << 8 : 1u << 10;

  for (size_t size = 16; size <= 128; size <<= 1)
  {
    test_alloc_dealloc<ZeroMem::NoZero>(count_small, size, false);
    test_alloc_dealloc<ZeroMem::NoZero>(count_small, size, true);
    test_alloc_dealloc<ZeroMem::YesZero>(count_small, size, false);
    test_alloc_dealloc<ZeroMem::YesZero>(count_small, size, true);
  }

  for (size_t size = 1 << 12; size <= 1 << 17; size <<= 1)
  {
    test_alloc_dealloc<ZeroMem::NoZero>(count_large, size, false);
    test_alloc_dealloc<ZeroMem::NoZero>(count_large, size, true);
    test_alloc_dealloc<ZeroMem::YesZero>(count_large, size, false);
    test_alloc_dealloc<ZeroMem::YesZero>(count_large, size, true);
  }

  return 0;
}
