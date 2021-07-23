#include <snmalloc_core.h>
#include <test/setup.h>

// Specify using own
#define SNMALLOC_EXTERNAL_THREAD_ALLOC

#include "backend/globalconfig.h"

namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::Globals>;
}

using namespace snmalloc;

class ThreadAllocExternal
{
public:
  static Alloc& get()
  {
    static thread_local Alloc alloc;
    return alloc;
  }
};

#include <snmalloc_front.h>

int main()
{
  setup();
  ThreadAlloc::get().init();

  auto& a = ThreadAlloc::get();

  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = a.alloc(i);

    a.dealloc(r1);
  }

  ThreadAlloc::get().teardown();

  // This checks that the scoped allocator does not call
  // register clean up, as this configuration will fault
  // if that occurs.
  auto a2 = ScopedAllocator();
  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = a2->alloc(i);

    a2->dealloc(r1);
  }
}
