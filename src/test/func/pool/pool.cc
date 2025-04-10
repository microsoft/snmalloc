#include <array>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <test/opt.h>
#include <test/setup.h>
#include <unordered_set>

using namespace snmalloc;

struct PoolAEntry : Pooled<PoolAEntry>
{
  int field;

  PoolAEntry() : field(1){};
};

using PoolA = Pool<PoolAEntry>;

struct PoolBEntry : Pooled<PoolBEntry>
{
  int field;

  PoolBEntry() : field(0){};
};

using PoolB = Pool<PoolBEntry>;

struct PoolLargeEntry : Pooled<PoolLargeEntry>
{
  std::array<int, 2'000'000> payload;

  PoolLargeEntry()
  {
    printf(".");
    fflush(stdout);
    payload[0] = 1;
    printf("first %d\n", payload[0]);
    payload[1'999'999] = 1;
    printf("last %d\n", payload[1'999'999]);
  };
};

using PoolLarge = Pool<PoolLargeEntry>;

struct PoolSortEntry : Pooled<PoolSortEntry>
{
  int field;

  PoolSortEntry() : field(1){};
};

using PoolSort = Pool<PoolSortEntry>;

void test_alloc()
{
  auto ptr = PoolA::acquire();
  SNMALLOC_CHECK(ptr != nullptr);
  // Pool allocations should not be visible to debug_check_empty.
  snmalloc::debug_check_empty<Alloc::Config>();
  PoolA::release(ptr);
}

void test_constructor()
{
  auto ptr1 = PoolA::acquire();
  SNMALLOC_CHECK(ptr1 != nullptr);
  SNMALLOC_CHECK(ptr1->field == 1);

  auto ptr2 = PoolB::acquire();
  SNMALLOC_CHECK(ptr2 != nullptr);
  SNMALLOC_CHECK(ptr2->field == 0);

  PoolA::release(ptr1);
  PoolB::release(ptr2);
}

void test_alloc_many()
{
  constexpr size_t count = 16'000'000 / MIN_CHUNK_SIZE;

  std::unordered_set<PoolAEntry*> allocated;

  for (size_t i = 0; i < count; ++i)
  {
    auto ptr = PoolA::acquire();
    SNMALLOC_CHECK(ptr != nullptr);
    allocated.insert(ptr);
  }

  for (auto ptr : allocated)
  {
    PoolA::release(ptr);
  }
}

void test_double_alloc()
{
  auto ptr1 = PoolA::acquire();
  SNMALLOC_CHECK(ptr1 != nullptr);
  auto ptr2 = PoolA::acquire();
  SNMALLOC_CHECK(ptr2 != nullptr);
  SNMALLOC_CHECK(ptr1 != ptr2);
  PoolA::release(ptr2);
  auto ptr3 = PoolA::acquire();
  // The following check assumes a stack discipline for acquire/release.
  // Placing it first in the list of tests means, there is a single element
  // and thus it works for both stack and queue.
  SNMALLOC_CHECK(ptr2 == ptr3);
  PoolA::release(ptr1);
  PoolA::release(ptr3);
}

void test_different_alloc()
{
  auto ptr1 = PoolA::acquire();
  SNMALLOC_CHECK(ptr1 != nullptr);
  PoolA::release(ptr1);
  auto ptr2 = PoolB::acquire();
  SNMALLOC_CHECK(ptr2 != nullptr);
  SNMALLOC_CHECK(static_cast<void*>(ptr1) != static_cast<void*>(ptr2));
  PoolB::release(ptr2);
}

void test_iterator()
{
  PoolAEntry* before_iteration_ptr = PoolA::acquire();

  PoolAEntry* ptr = nullptr;
  while ((ptr = PoolA::iterate(ptr)) != nullptr)
  {
    ptr->field = 2;
  }

  SNMALLOC_CHECK(before_iteration_ptr->field == 2);

  PoolAEntry* after_iteration_ptr = PoolA::acquire();

  SNMALLOC_CHECK(after_iteration_ptr->field == 2);

  PoolA::release(before_iteration_ptr);
  PoolA::release(after_iteration_ptr);
}

void test_large()
{
  printf(".");
  fflush(stdout);
  PoolLargeEntry* p = PoolLarge::acquire();
  printf(".");
  fflush(stdout);
  PoolLarge::release(p);
  printf(".");
  fflush(stdout);
}

/**
 * This test confirms that the pool is sorted consistently with
 * respect to the iterator after a call to sort.
 */
void test_sort()
{
  auto position = [](PoolSortEntry* ptr) {
    size_t i = 0;
    auto curr = PoolSort::iterate();
    while (ptr != curr)
    {
      curr = PoolSort::iterate(curr);
      ++i;
    }
    return i;
  };

  // This test checks that `sort` puts the elements in the right order,
  // so it is the same as if they had been allocated in that order.
  auto a1 = PoolSort::acquire();
  auto a2 = PoolSort::acquire();
  auto a3 = PoolSort::acquire();

  auto position1 = position(a1);
  auto position2 = position(a2);
  auto position3 = position(a3);

  PoolSort::release(a1);
  PoolSort::release(a2);
  PoolSort::release(a3);
  PoolSort::sort();

  // Repeat the test to ensure it re-establishes the order.
  for (size_t i = 0; i < 12; i++)
  {
    auto b1 = PoolSort::acquire();
    auto b2 = PoolSort::acquire();
    auto b3 = PoolSort::acquire();

    auto new_position1 = position(b1);
    auto new_position2 = position(b2);
    auto new_position3 = position(b3);

    SNMALLOC_CHECK(new_position1 == position1);
    SNMALLOC_CHECK(new_position2 == position2);
    SNMALLOC_CHECK(new_position3 == position3);

    // Release in either order.
    switch (i % 6)
    {
      case 0:
        PoolSort::release(b1);
        PoolSort::release(b2);
        PoolSort::release(b3);
        break;
      case 1:
        PoolSort::release(b1);
        PoolSort::release(b3);
        PoolSort::release(b2);
        break;
      case 2:
        PoolSort::release(b2);
        PoolSort::release(b1);
        PoolSort::release(b3);
        break;
      case 3:
        PoolSort::release(b2);
        PoolSort::release(b3);
        PoolSort::release(b1);
        break;
      case 4:
        PoolSort::release(b3);
        PoolSort::release(b1);
        PoolSort::release(b2);
        break;
      case 5:
        PoolSort::release(b3);
        PoolSort::release(b2);
        PoolSort::release(b1);
        break;
    }

    PoolSort::sort();
  }
}

int main(int argc, char** argv)
{
  setup();
#ifdef USE_SYSTEMATIC_TESTING
  opt::Opt opt(argc, argv);
  size_t seed = opt.is<size_t>("--seed", 0);
  Virtual::systematic_bump_ptr() += seed << 17;
#else
  UNUSED(argc, argv);
#endif

  test_double_alloc();
  std::cout << "test_double_alloc passed" << std::endl;
  test_alloc();
  std::cout << "test_alloc passed" << std::endl;
  test_constructor();
  std::cout << "test_constructor passed" << std::endl;
  test_alloc_many();
  std::cout << "test_alloc_many passed" << std::endl;
  test_different_alloc();
  std::cout << "test_different_alloc passed" << std::endl;
  test_iterator();
  std::cout << "test_iterator passed" << std::endl;
  test_large();
  std::cout << "test_large passed" << std::endl;
  test_sort();
  std::cout << "test_sort passed" << std::endl;
  return 0;
}
