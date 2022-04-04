#pragma once

#include "../backend/globalconfig.h"

#if defined(SNMALLOC_EXTERNAL_THREAD_ALLOC)
#  define SNMALLOC_THREAD_TEARDOWN_DEFINED
#endif

#if defined(SNMALLOC_USE_THREAD_CLEANUP)
#  if defined(SNMALLOC_THREAD_TEARDOWN_DEFINED)
#    error At most one out of method of thread teardown can be specified.
#  else
#    define SNMALLOC_THREAD_TEARDOWN_DEFINED
#  endif
#endif

#if defined(SNMALLOC_USE_PTHREAD_DESTRUCTORS)
#  if defined(SNMALLOC_THREAD_TEARDOWN_DEFINED)
#    error At most one out of method of thread teardown can be specified.
#  else
#    include <pthread.h>
#    define SNMALLOC_THREAD_TEARDOWN_DEFINED
#  endif
#endif

#if !defined(SNMALLOC_THREAD_TEARDOWN_DEFINED)
#  define SNMALLOC_USE_CXX_THREAD_DESTRUCTORS
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
  inline void register_clean_up()
  {
    error("Critical Error: This should never be called.");
  }
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif
#else
  /**
   * Holds the thread local state for the allocator.  The state is constant
   * initialised, and has no direct dectructor.  Instead snmalloc will call
   * `register_clean_up` on the slow path for bringing up thread local state.
   * This is responsible for calling `teardown`, which effectively destructs the
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

#  ifdef SNMALLOC_USE_PTHREAD_DESTRUCTORS
  /**
   * Used to give correct signature to teardown required by pthread_key.
   */
  inline void pthread_cleanup(void*)
  {
    ThreadAlloc::get().teardown();
  }

  /**
   * Used to give correct signature to teardown required by atexit.
   */
  inline void pthread_cleanup_main_thread()
  {
    ThreadAlloc::get().teardown();
  }

  /**
   * Used to give correct signature to the pthread call for the Singleton class.
   */
  inline void pthread_create(pthread_key_t* key) noexcept
  {
    pthread_key_create(key, &pthread_cleanup);
    // Main thread does not call pthread_cleanup if `main` returns or `exit` is
    // called, so use an atexit handler to guarantee that the cleanup is run at
    // least once.  If the main thread exits with `pthread_exit` then it will be
    // called twice but this case is already handled because other destructors
    // can cause the per-thread allocator to be recreated.
    atexit(&pthread_cleanup_main_thread);
  }

  /**
   * Performs thread local teardown for the allocator using the pthread library.
   *
   * This removes the dependence on the C++ runtime.
   */
  inline void register_clean_up()
  {
    Singleton<pthread_key_t, &pthread_create> p_key;
    // We need to set a non-null value, so that the destructor is called,
    // we never look at the value.
    static char p_teardown_val = 1;
    pthread_setspecific(p_key.get(), &p_teardown_val);
#    ifdef SNMALLOC_TRACING
    message<1024>("Using pthread clean up");
#    endif
  }
#  elif defined(SNMALLOC_USE_CXX_THREAD_DESTRUCTORS)
  /**
   * This function is called by each thread once it starts using the
   * thread local allocator.
   *
   * This implementation depends on nothing outside of a working C++
   * environment and so should be the simplest for initial bringup on an
   * unsupported platform.
   */
  inline void register_clean_up()
  {
    static thread_local OnDestruct dummy(
      []() { ThreadAlloc::get().teardown(); });
    UNUSED(dummy);
#    ifdef SNMALLOC_TRACING
    message<1024>("Using C++ destructor clean up");
#    endif
  }
#  endif
#endif
} // namespace snmalloc

#ifdef SNMALLOC_USE_THREAD_CLEANUP
/**
 * Entry point that allows libc to call into the allocator for per-thread
 * cleanup.
 */
SNMALLOC_USED_FUNCTION
inline void _malloc_thread_cleanup()
{
  snmalloc::ThreadAlloc::get().teardown();
}

namespace snmalloc
{
  /**
   * No-op version of register_clean_up.  This is called unconditionally by
   * globalconfig but is not necessary when using a libc hook.
   */
  inline void register_clean_up() {}
}
#endif
