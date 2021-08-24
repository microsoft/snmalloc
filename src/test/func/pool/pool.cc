#include <snmalloc.h>
#include <test/opt.h>
#include <test/setup.h>
#include <unordered_set>

using namespace snmalloc;

struct PoolAEntry : Pooled<PoolAEntry>
{
  size_t field;

  PoolAEntry() : field(1){};
};

using PoolA = Pool<PoolAEntry, Alloc::StateHandle>;

struct PoolBEntry : Pooled<PoolBEntry>
{
  size_t field;

  PoolBEntry() : field(0){};
  PoolBEntry(size_t f) : field(f){};
};

using PoolB = Pool<PoolBEntry, Alloc::StateHandle>;

void test_alloc()
{
  auto ptr = PoolA::acquire();
  SNMALLOC_CHECK(ptr != nullptr);
  // Pool allocations should not be visible to debug_check_empty.
  snmalloc::debug_check_empty<Alloc::StateHandle>();
}

void test_constructor()
{
  auto ptr1 = PoolA::acquire();
  SNMALLOC_CHECK(ptr1 != nullptr);
  SNMALLOC_CHECK(ptr1->field == 1);

  auto ptr2 = PoolB::acquire();
  SNMALLOC_CHECK(ptr2 != nullptr);
  SNMALLOC_CHECK(ptr2->field == 0);

  auto ptr3 = PoolB::acquire(1);
  SNMALLOC_CHECK(ptr3 != nullptr);
  SNMALLOC_CHECK(ptr3->field == 1);
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

void test_alloc_dealloc()
{
  auto ptr = PoolA::acquire();
  SNMALLOC_CHECK(ptr != nullptr);
  PoolA::release(ptr);
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
  SNMALLOC_CHECK(ptr2 == ptr3);
}

void test_different_alloc()
{
  auto ptr1 = PoolA::acquire();
  SNMALLOC_CHECK(ptr1 != nullptr);
  PoolA::release(ptr1);
  auto ptr2 = PoolB::acquire();
  SNMALLOC_CHECK(ptr2 != nullptr);
  SNMALLOC_CHECK(static_cast<void*>(ptr1) != static_cast<void*>(ptr2));
}

int main(int argc, char** argv)
{
  setup();
#ifdef USE_SYSTEMATIC_TESTING
  opt::Opt opt(argc, argv);
  size_t seed = opt.is<size_t>("--seed", 0);
  Virtual::systematic_bump_ptr() += seed << 17;
#else
  UNUSED(argc);
  UNUSED(argv);
#endif

  test_alloc();
  test_constructor();
  test_alloc_many();
  test_alloc_dealloc();
  test_double_alloc();
  test_different_alloc();
  return 0;
}
