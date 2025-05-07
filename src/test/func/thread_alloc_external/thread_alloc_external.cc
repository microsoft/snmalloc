#ifdef SNMALLOC_USE_PTHREAD_DESTRUCTORS
#  undef SNMALLOC_USE_PTHREAD_DESTRUCTORS
#endif
#ifdef SNMALLOC_USE_CXX11_THREAD_ATEXIT_DIRECT
#  undef SNMALLOC_USE_CXX11_THREAD_ATEXIT_DIRECT
#endif
#ifdef SNMALLOC_USE_CXX11_DESTRUCTORS
#  undef SNMALLOC_USE_CXX11_DESTRUCTORS
#endif

#include <new>
#include <snmalloc/snmalloc_core.h>
#include <test/setup.h>

// Specify using own
#define SNMALLOC_EXTERNAL_THREAD_ALLOC

#include <snmalloc/backend/globalconfig.h>

namespace snmalloc
{
  using Config = snmalloc::StandardConfigClientMeta<NoClientMetaDataProvider>;
  using Alloc = snmalloc::Allocator<Config>;
}

using namespace snmalloc;

class ThreadAllocExternal
{
public:
  static Alloc*& get_inner()
  {
    static thread_local Alloc* alloc;
    return alloc;
  }

  static Alloc& get()
  {
    return *get_inner();
  }
};

#include <snmalloc/snmalloc_front.h>

void allocator_thread_init(void)
{
  void* aptr;
  {
    // Create bootstrap allocator
    auto a = snmalloc::ScopedAllocator();
    // Create storage for the thread-local allocator
    aptr = a->alloc(sizeof(snmalloc::Alloc));
  }
  // Initialize the thread-local allocator
  ThreadAllocExternal::get_inner() = new (aptr) snmalloc::Alloc();
}

void allocator_thread_cleanup(void)
{
  // Teardown the thread-local allocator
  ThreadAlloc::teardown();
  // Need a bootstrap allocator to deallocate the thread-local allocator
  auto a = snmalloc::ScopedAllocator();
  // Deallocate the storage for the thread local allocator
  a->dealloc(ThreadAllocExternal::get_inner());
}

int main()
{
  setup();
  allocator_thread_init();

  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = snmalloc::alloc(i);

    snmalloc::dealloc(r1);
  }

  snmalloc::debug_teardown();

  // This checks that the scoped allocator does not call
  // register clean up, as this configuration will fault
  // if that occurs.
  auto a2 = ScopedAllocator();
  for (size_t i = 0; i < 1000; i++)
  {
    auto r1 = a2->alloc(i);

    a2->dealloc(r1);
  }

  allocator_thread_cleanup();
}
