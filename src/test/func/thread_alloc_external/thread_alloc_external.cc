#include <snmalloc_core.h>
#include <test/setup.h>

// Specify using own
#define SNMALLOC_EXTERNAL_THREAD_ALLOC
namespace snmalloc
{
  using Alloc = snmalloc::LocalAllocator<snmalloc::Globals>;
}

using namespace snmalloc;

class ThreadAllocUntyped
{
public:
  static void* get()
  {
    static thread_local bool inited = false;
    static thread_local Alloc alloc;
    if (!inited)
    {
      alloc.init();
      inited = true;
    }

    return &alloc;
  }
};

#include <snmalloc_front.h>

int main()
{
  setup();

  auto a = ThreadAlloc::get();

  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = a->alloc(i);

    a->dealloc(r1);
  }
}
