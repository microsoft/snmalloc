#include <test/setup.h>

#define SNMALLOC_EXTERNAL_THREAD_ALLOC
#include <mem/globalalloc.h>
using namespace snmalloc;

class ThreadAllocUntyped
{
public:
  static void* get()
  {
    static thread_local void* alloc = nullptr;
    if (alloc != nullptr)
    {
      return alloc;
    }
 
    alloc = current_alloc_pool()->acquire();
    return alloc;
  }
};

#include <snmalloc.h>

int main()
{
  setup();

  MemoryProviderStateMixin<DefaultPal> mp;

  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
 
  auto a = ThreadAlloc::get();

  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = a->alloc(100);
 
    a->dealloc(r1);
  }
}
