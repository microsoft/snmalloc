#include <snmalloc.h>
#include <test/measuretime.h>
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
      if (offset > 30)
        offset = 30;
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
    auto* alloc = ThreadAlloc::get();

    setup(r, alloc);

    DO_TIME("External pointer queries ", {
      for (size_t i = 0; i < 10000000; i++)
      {
        size_t rand = (size_t)r.next();
        size_t oid = rand & (((size_t)1 << count_log) - 1);
        size_t* external_ptr = objects[oid];
        size_t size = *external_ptr;
        size_t offset = (size >> 4) * (rand & 15);
        size_t interior_ptr = ((size_t)external_ptr) + offset;
        void* calced_external = Alloc::external_pointer((void*)interior_ptr);
        if (calced_external != external_ptr)
          abort();
      }
    });

    teardown(alloc);
  }
}

int main(int, char**)
{
  xoroshiro::p128r64 r;
#if NDEBUG
  size_t nn = 30;
#else
  size_t nn = 3;
#endif

  for (size_t n = 0; n < nn; n++)
    test::test_external_pointer(r);
  return 0;
}
