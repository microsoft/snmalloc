#include <test/measuretime.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>

using namespace snmalloc;

static constexpr size_t ALLOC_SIZE = 800 * 1024; // 800 KB
static constexpr size_t ITERATIONS = 100000;

void test_alloc_dealloc_cycle()
{
  {
    MeasureTime m;
    m << "Alloc/dealloc 800KB x " << ITERATIONS;

    for (size_t i = 0; i < ITERATIONS; i++)
    {
      void* p = snmalloc::alloc(ALLOC_SIZE);
      SNMALLOC_CHECK(p != nullptr);
      snmalloc::dealloc(p);
    }
  }

  snmalloc::debug_check_empty();
}

void test_batch_alloc_then_dealloc()
{
  static constexpr size_t BATCH = 128;

  void* ptrs[BATCH];

  MeasureTime m;
  m << "Batch alloc then dealloc 800KB x " << BATCH;
  for (size_t j = 0; j < ITERATIONS / BATCH; j++)
  {
    for (size_t i = 0; i < BATCH; i++)
    {
      ptrs[i] = snmalloc::alloc(ALLOC_SIZE);
      SNMALLOC_CHECK(ptrs[i] != nullptr);
    }

    for (size_t i = 0; i < BATCH; i++)
    {
      snmalloc::dealloc(ptrs[i]);
    }
  }

  snmalloc::debug_check_empty();
}

void test_alloc_dealloc_with_touch()
{
  {
    MeasureTime m;
    m << "Alloc/touch/dealloc 800KB x " << ITERATIONS;

    for (size_t i = 0; i < ITERATIONS; i++)
    {
      char* p = static_cast<char*>(snmalloc::alloc(ALLOC_SIZE));
      SNMALLOC_CHECK(p != nullptr);
      // Touch every 4KiB  and last bytes to ensure pages are faulted in
      for (size_t offset = 0; offset < ALLOC_SIZE; offset += 4096)
      {
        p[offset] = 1;
      }
      snmalloc::dealloc(p);
    }
  }

  snmalloc::debug_check_empty();
}

int main(int, char**)
{
  setup();

  test_alloc_dealloc_cycle();
  test_batch_alloc_then_dealloc();
  test_alloc_dealloc_with_touch();

  return 0;
}
