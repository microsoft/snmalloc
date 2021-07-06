#pragma once

#include "../ds/helpers.h"
#include "globalconfig.h"
#include "localalloc.h"

#ifdef SNMALLOC_USE_PTHREAD_DESTRUCTOR
#  include <pthread.h>
#endif

#if defined(SNMALLOC_USE_PTHREAD_DESTRUCTOR) && \
  defined(SNMALLOC_USE_THREAD_CLEANUP)
#error At most one out of SNMALLOC_USE_THREAD_CLEANUP and SNMALLOC_USE_PTHREAD_DESTRUCTOR may be defined.
#endif

extern "C" void _malloc_thread_cleanup();

namespace snmalloc
{
#ifdef SNMALLOC_EXTERNAL_THREAD_ALLOC
  /**
   * Version of the `ThreadAlloc` interface that does no management of thread
   * local state.
   *
   * It assumes that Alloc has been defined, and `ThreadAllocExternal` class
   * has access to snmalloc_core.h.
   */
  class ThreadAlloc
  {
  protected:
    static void register_cleanup() {}

  public:
    static SNMALLOC_FAST_PATH Alloc& get()
    {
      return ThreadAllocExternal::get();
    }
  };

  /**
   * Function passed as a template parameter to `Allocator` to allow lazy
   * replacement.  There is nothing to initialise in this case, so we expect
   * this to never be called.
   */
#  ifdef _MSC_VER
// 32Bit Windows release MSVC is determining this as having unreachable code for
// f(nullptr), which is true.  But other platforms don't. Disabling the warning
// seems simplist.
#    pragma warning(push)
#    pragma warning(disable : 4702)
#  endif
  SNMALLOC_FAST_PATH void register_clean_up()
  {
    error("Critical Error: This should never be called.");
  }
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif
#else

  /**
   * Holds the thread local state for the allocator.  The state is constant initialised, and has no
   * direct dectructor.  Instead snmalloc will call `register_clean_up` on the slow path for bringing
   * up thread local state. This is responsible for calling `teardown`, which effectively destructs the
   * data structure, but in a way that allow it to still be used.
   */
  class ThreadAlloc
  {
  public:
    /**
     * Handle on thread local allocator
     *
     * This structure will self initialise if it has not been called yet.
     * It can be used during thread teardown, but its performance will be
     * less good.
     */
    static SNMALLOC_FAST_PATH Alloc& get()
    {
      SNMALLOC_REQUIRE_CONSTINIT static thread_local Alloc alloc;
      return alloc;
    }
  };

# ifdef SNMALLOC_USE_PTHREAD_DESTRUCTOR
  inline void pthread_cleanup(void*)
  {
    ThreadAlloc::get().teardown();
  }
  inline pthread_key_t pthread_create() noexcept
  {
    pthread_key_t key;
    pthread_key_create(&key, &pthread_cleanup);
    return key;
  }
  inline SNMALLOC_FAST_PATH void register_clean_up()
  { 
    Singleton<pthread_key_t, &pthread_create> p_key;
    pthread_setspecific(p_key.get(), (void*)1);
  }
#  else
  /**
   * This function is called by each thread once it starts using the
   * thread local allocator.
   *
   * This implementation depends on nothing outside of a working C++
   * environment and so should be the simplest for initial bringup on an
   * unsupported platform.  
   */
  inline SNMALLOC_FAST_PATH void register_clean_up()
  {
    static thread_local OnDestruct dummy([](){ ThreadAlloc::get().teardown();});
    UNUSED(dummy);
  }
#  endif
#endif
} // namespace snmalloc

#ifdef SNMALLOC_USE_THREAD_CLEANUP
/**
 * Entry point that allows libc to call into the allocator for per-thread
 * cleanup.
 */
void _malloc_thread_cleanup()
{
  ThreadAlloc::get().teardown();
}
#endif
