#include <snmalloc.h>
#include <test/measuretime.h>
#include <test/setup.h>
#include <unordered_set>

using namespace snmalloc;

template<ZeroMem zero_mem>
void test_alloc_dealloc(size_t count, size_t size, bool write)
{
  auto* alloc = ThreadAlloc::get();

  DO_TIME(
    "Count: " << std::setw(6) << count << ", Size: " << std::setw(6) << size
              << ", ZeroMem: " << (zero_mem == YesZero) << ", Write: " << write,
    {
      std::unordered_set<void*> set;

      // alloc 1.5x objects
      for (size_t i = 0; i < ((count * 3) / 2); i++)
      {
        void* p = alloc->alloc<zero_mem>(size);
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
        alloc->dealloc(p, size);
        set.erase(it);
        SNMALLOC_CHECK(set.find(p) == set.end());
      }

      // alloc 1x objects
      for (size_t i = 0; i < count; i++)
      {
        void* p = alloc->alloc<zero_mem>(size);
        SNMALLOC_CHECK(set.find(p) == set.end());

        if (write)
          *(int*)p = 4;

        set.insert(p);
      }

      // free everything
      while (!set.empty())
      {
        auto it = set.begin();
        alloc->dealloc(*it, size);
        set.erase(it);
      }
    });

  current_alloc_pool()->debug_check_empty();
}

int main(int, char**)
{
  setup();

  for (size_t size = 16; size <= 128; size <<= 1)
  {
    test_alloc_dealloc<NoZero>(1 << 15, size, false);
    test_alloc_dealloc<NoZero>(1 << 15, size, true);
    test_alloc_dealloc<YesZero>(1 << 15, size, false);
    test_alloc_dealloc<YesZero>(1 << 15, size, true);
  }

  for (size_t size = 1 << 12; size <= 1 << 17; size <<= 1)
  {
    test_alloc_dealloc<NoZero>(1 << 10, size, false);
    test_alloc_dealloc<NoZero>(1 << 10, size, true);
    test_alloc_dealloc<YesZero>(1 << 10, size, false);
    test_alloc_dealloc<YesZero>(1 << 10, size, true);
  }

  return 0;
}
