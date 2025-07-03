#include <snmalloc/snmalloc.h>
#include <test/measuretime.h>
#include <test/setup.h>
#include <unordered_set>

using namespace snmalloc;

template<typename Conts>
void test_alloc_dealloc(size_t count, size_t size, bool write)
{
  {
    MeasureTime m;
    m << "Count: " << std::setw(6) << count << ", Size: " << std::setw(6)
      << size
      << ", ZeroMem: " << std::is_same_v<Conts, Zero> << ", Write: " << write;

    std::unordered_set<void*> set;

    // alloc 1.5x objects
    for (size_t i = 0; i < ((count * 3) / 2); i++)
    {
      void* p = snmalloc::alloc<Conts>(size);
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
      void* p = snmalloc::alloc<Conts>(size);
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

int main(int, char**)
{
  setup();

  for (size_t size = 16; size <= 128; size <<= 1)
  {
    test_alloc_dealloc<Uninit>(1 << 15, size, false);
    test_alloc_dealloc<Uninit>(1 << 15, size, true);
    test_alloc_dealloc<Zero>(1 << 15, size, false);
    test_alloc_dealloc<Zero>(1 << 15, size, true);
  }

  for (size_t size = 1 << 12; size <= 1 << 17; size <<= 1)
  {
    test_alloc_dealloc<Uninit>(1 << 10, size, false);
    test_alloc_dealloc<Uninit>(1 << 10, size, true);
    test_alloc_dealloc<Zero>(1 << 10, size, false);
    test_alloc_dealloc<Zero>(1 << 10, size, true);
  }

  return 0;
}
