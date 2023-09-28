#pragma once

#include "singleton.h"

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

namespace snmalloc
{
  /**
   * @brief Thread local that has a cleanup function that can be registered
   * off the fast path
   *
   * @tparam A - the type of the thread local
   *
   * @details This is used in the following way
   *
   *   ThreadLocal<Alloc>::get().alloc(16);
   *
   *   Inside, the call to alloc if it detects it is the first time the
   * structure is being used, then the Alloc should call
   *
   *     ThreadLocal<Alloc>::register_cleanup();
   *
   *   This means that the thread local will be cleaned up when the thread
   * exits. The detecting of the first time it is used can be moved of the fast
   * path, and conflated with other initial checks like the thread local free
   * list is empty for that size class.
   *
   *   There are multiple configurations for various platformat that are given
   * below.
   */
  template<typename A>
  class ThreadLocal
  {
  public:
    SNMALLOC_FAST_PATH static A& get()
    {
      SNMALLOC_REQUIRE_CONSTINIT static thread_local A alloc;
      return alloc;
    }

    static void register_cleanup();
  };

#ifdef SNMALLOC_USE_PTHREAD_DESTRUCTORS
  /**
   * Used to give correct signature to teardown required by pthread_key.
   */
  template<typename A>
  inline void pthread_cleanup(void*)
  {
    ThreadLocal<A>::get().teardown();
  }

  /**
   * Used to give correct signature to teardown required by atexit.
   */
  template<typename A>
  inline void pthread_cleanup_main_thread()
  {
    ThreadLocal<A>::get().teardown();
  }

  /**
   * Used to give correct signature to the pthread call for the Singleton class.
   */
  template<typename A>
  inline void pthread_create(pthread_key_t* key) noexcept
  {
    pthread_key_create(key, &pthread_cleanup<A>);
    // Main thread does not call pthread_cleanup if `main` returns or `exit` is
    // called, so use an atexit handler to guarantee that the cleanup is run at
    // least once.  If the main thread exits with `pthread_exit` then it will be
    // called twice but this case is already handled because other destructors
    // can cause the per-thread allocator to be recreated.
    atexit(&pthread_cleanup_main_thread<A>);
  }

  /**
   * Performs thread local teardown for the allocator using the pthread library.
   *
   * This removes the dependence on the C++ runtime.
   */
  template<typename A>
  inline void ThreadLocal<A>::register_cleanup()
  {
    Singleton<pthread_key_t, &pthread_create<A>> p_key;
    // We need to set a non-null value, so that the destructor is called,
    // we never look at the value.
    static char p_teardown_val = 1;
    pthread_setspecific(p_key.get(), &p_teardown_val);
#  ifdef SNMALLOC_TRACING
    message<1024>("Using pthread clean up");
#  endif
  }
#elif defined(SNMALLOC_USE_CXX_THREAD_DESTRUCTORS)
  /**
   * This function is called by each thread once it starts using the
   * thread local allocator.
   *
   * This implementation depends on nothing outside of a working C++
   * environment and so should be the simplest for initial bringup on an
   * unsupported platform.
   */
  template<typename A>
  void ThreadLocal<A>::register_cleanup()
  {
    static thread_local OnDestruct dummy(
      []() { ThreadLocal<A>::get().teardown(); });
    UNUSED(dummy);
#  ifdef SNMALLOC_TRACING
    message<1024>("Using C++ destructor clean up");
#  endif
  }
#elif defined(SNMALLOC_USE_THREAD_CLEANUP)
  /**
   * Entry point that allows libc to call into the allocator for per-thread
   * cleanup.
   */
  SNMALLOC_USED_FUNCTION
  inline void _malloc_thread_cleanup()
  {
    // This needs to traverse the list of allocators
    // thread locals and call each on.
    abort();
  }

  /**
   * No-op version of register_clean_up.  This is called unconditionally by
   * globalconfig but is not necessary when using a libc hook.
   */
  template<typename A>
  inline void ThreadLocal<A>::register_clean_up()
  {
    // Add ThreadLocal<A>::teardown() to the list of thread_cleanup calls.
    abort();
  }
#endif
} // namespace snmalloc