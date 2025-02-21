#include <snmalloc/snmalloc.h>
#include <test/measuretime.h>
#include <test/setup.h>
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
      if constexpr (DefaultPal::address_bits > 32)
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

  void test_external_pointer(xoroshiro::p128r64& r)
  {
    // This is very slow on Windows at the moment.  Until this is fixed, help
    // CI terminate.
#if defined(NDEBUG) && !defined(_MSC_VER)
    static constexpr size_t iterations = 10000000;
#else
#  ifdef _MSC_VER
    // Windows Debug build is very slow on this test.
    // Reduce complexity to balance CI times.
    static constexpr size_t iterations = 50000;
#  else
    static constexpr size_t iterations = 100000;
#  endif
#endif
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
        void* calced_external = snmalloc::external_pointer(interior_ptr);
        if (calced_external != external_ptr)
        {
          abort();
        }
      }
    }

    teardown();
  }
}

int main(int, char**)
{
  setup();

  xoroshiro::p128r64 r;

  size_t nn = snmalloc::Debug ? 30 : 3;

  for (size_t n = 0; n < nn; n++)
    test::test_external_pointer(r);
  return 0;
}
