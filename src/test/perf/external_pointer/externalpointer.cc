#include <snmalloc.h>
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

  NOINLINE void setup(xoroshiro::p128r64& r, Alloc* alloc)
  {
    for (size_t i = 0; i < count; i++)
    {
      size_t rand = (size_t)r.next();
      size_t offset = bits::clz(rand);
      if constexpr (bits::is64())
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
      objects[i] = (size_t*)alloc->alloc(size);
      // Store allocators size for this object
      *objects[i] = Alloc::alloc_size(objects[i]);
    }
  }

  NOINLINE void teardown(Alloc* alloc)
  {
    // Deallocate everything
    for (size_t i = 0; i < count; i++)
    {
      alloc->dealloc(objects[i]);
    }

    current_alloc_pool()->debug_check_empty();
  }

  void test_external_pointer(xoroshiro::p128r64& r)
  {
    auto alloc = ThreadAlloc::get();
#ifdef NDEBUG
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
    setup(r, alloc);

    DO_TIME("External pointer queries ", {
      for (size_t i = 0; i < iterations; i++)
      {
        size_t rand = (size_t)r.next();
        size_t oid = rand & (((size_t)1 << count_log) - 1);
        size_t* external_ptr = objects[oid];
        size_t size = *external_ptr;
        size_t offset = (size >> 4) * (rand & 15);
        void* interior_ptr = pointer_offset(external_ptr, offset);
        void* calced_external = Alloc::external_pointer(interior_ptr);
        if (calced_external != external_ptr)
          abort();
      }
    });

    teardown(alloc);
  }
}

int main(int, char**)
{
  setup();

  xoroshiro::p128r64 r;
#ifdef NDEBUG
  size_t nn = 30;
#else
  size_t nn = 3;
#endif

  for (size_t n = 0; n < nn; n++)
    test::test_external_pointer(r);
  return 0;
}
