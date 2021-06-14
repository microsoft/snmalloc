#include <test/setup.h>

#define SNMALLOC_EXTERNAL_THREAD_ALLOC
#include <mem/fastalloc.h>
#include <mem/globalconfig.h>
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

#include <snmalloc.h>

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
