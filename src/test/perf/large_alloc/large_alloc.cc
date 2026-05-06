#include <test/measuretime.h>
#include <test/perf_setup.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>

using namespace snmalloc;

static constexpr size_t ALLOC_SIZE = 800 * 1024; // 800 KB

void test_alloc_dealloc_cycle(size_t iterations)
{
  {
    MeasureTime m;
    m << "Alloc/dealloc 800KB x " << iterations;

    for (size_t i = 0; i < iterations; i++)
    {
      void* p = snmalloc::alloc(ALLOC_SIZE);
      SNMALLOC_CHECK(p != nullptr);
      snmalloc::dealloc(p);
    }
  }

  snmalloc::debug_check_empty();
}

void test_batch_alloc_then_dealloc(size_t iterations)
{
  static constexpr size_t BATCH = 128;

  void* ptrs[BATCH];

  MeasureTime m;
  m << "Batch alloc then dealloc 800KB x " << BATCH;
  for (size_t j = 0; j < iterations / BATCH; j++)
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

void test_alloc_dealloc_with_touch(size_t iterations)
{
  {
    MeasureTime m;
    m << "Alloc/touch/dealloc 800KB x " << iterations;

    for (size_t i = 0; i < iterations; i++)
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

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);
  // Each test does alloc/dealloc cycles driven by `iterations`. The
  // batch test divides by BATCH=128, so the smoke value is chosen so
  // that `smoke / 128 >= 1` (i.e. the batch test still runs at least
  // one full batch round).
  size_t iterations = snmalloc_test::perf_iterations(
    opt, SNMALLOC_TEST_NAME, /*default=*/100000, /*smoke=*/8192);

  test_alloc_dealloc_cycle(iterations);
  test_batch_alloc_then_dealloc(iterations);
  test_alloc_dealloc_with_touch(iterations);

  return 0;
}
