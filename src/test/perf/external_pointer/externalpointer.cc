#include <test/measuretime.h>
#include <test/opt.h>
#include <test/perf_setup.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>
#include <test/xoroshiro.h>
#include <unordered_set>

using namespace snmalloc;

namespace test
{
  static constexpr size_t count_log = 20;
  static constexpr size_t count = 1 << count_log;
  // Pre allocate all the objects
  size_t* objects[count];

  NOINLINE void setup(xoroshiro::p128r64& r)
  {
    for (size_t i = 0; i < count; i++)
    {
      size_t rand = (size_t)r.next();
      size_t offset = bits::clz(rand);
      if (snmalloc::pal_address_bits() > 32)
      {
        if (offset > 30)
          offset = 30;
      }
      else if (offset > 20)
        offset = 20;

      size_t size = (rand & 15) << offset;
      if (size < 16)
        size = 16;
      // store object
      objects[i] = (size_t*)snmalloc::alloc(size);
      if (objects[i] == nullptr)
        abort();
      // Store allocators size for this object
      *objects[i] = snmalloc::alloc_size(objects[i]);
    }
  }

  NOINLINE void teardown()
  {
    // Deallocate everything
    for (size_t i = 0; i < count; i++)
    {
      snmalloc::dealloc(objects[i]);
    }

    snmalloc::debug_check_empty();
  }

  void test_external_pointer(xoroshiro::p128r64& r, size_t iterations)
  {
    setup(r);

    {
      MeasureTime m;
      m << "External pointer queries ";
      for (size_t i = 0; i < iterations; i++)
      {
        size_t rand = (size_t)r.next();
        size_t oid = rand & (((size_t)1 << count_log) - 1);
        size_t* external_ptr = objects[oid];
        if (!snmalloc::is_owned(external_ptr))
          continue;
        size_t size = *external_ptr;
        size_t offset = (size >> 4) * (rand & 15);
        void* interior_ptr = pointer_offset(external_ptr, offset);
        void* calced_external =
          snmalloc::libc::__malloc_start_pointer(interior_ptr);
        if (calced_external != external_ptr)
        {
          abort();
        }
      }
    }

    teardown();
  }
}

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);

  // Default iteration count varies by build (Release runs many more
  // iterations). Smoke mode shrinks both to the smallest count that
  // still exercises every interior-pointer dispatch path.
  size_t cli_default;
  // This is very slow on Windows at the moment.  Until this is fixed, help
  // CI terminate.
#if defined(NDEBUG) && !defined(_MSC_VER)
  cli_default = 10000000;
#elif defined(_MSC_VER)
  // Windows Debug build is very slow on this test.
  // Reduce complexity to balance CI times.
  cli_default = 50000;
#else
  cli_default = 100000;
#endif
  size_t iterations = snmalloc_test::perf_iterations(
    opt, SNMALLOC_TEST_NAME, cli_default, /*smoke=*/10000);

  // Outer-repeat count: Debug repeats 30x to amortise setup, Release 3x.
  // Smoke shrinks both ends; one repeat is enough to hit every path
  // since `setup()` re-randomises the object table each call.
  size_t nn_default = snmalloc::Debug ? 30 : 3;
  size_t nn = snmalloc_test::perf_iterations(
    opt, SNMALLOC_TEST_NAME, nn_default, /*smoke=*/1);

  xoroshiro::p128r64 r;

  for (size_t n = 0; n < nn; n++)
    test::test_external_pointer(r, iterations);
  return 0;
}
