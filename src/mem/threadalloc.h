#pragma once

#include "../ds/helpers.h"
#include "fastalloc.h"
#include "globalconfig.h"

#if defined(SNMALLOC_USE_THREAD_DESTRUCTOR) && \
  defined(SNMALLOC_USE_THREAD_CLEANUP)
#error At most one out of SNMALLOC_USE_THREAD_CLEANUP and SNMALLOC_USE_THREAD_DESTRUCTOR may be defined.
#endif

extern "C" void _malloc_thread_cleanup();

namespace snmalloc
{
#ifdef SNMALLOC_EXTERNAL_THREAD_ALLOC
  /**
   * Version of the `ThreadAlloc` interface that does no management of thread
   * local state, and just assumes that "ThreadAllocUntyped::get" has been
   * declared before including snmalloc.h.  As it is included before, it cannot
   * know the allocator type, hence the casting.
   *
   * This class is used only when snmalloc is compiled as part of a runtime,
   * which has its own management of the thread local allocator pointer.
   */
  class ThreadAllocUntypedWrapper
  {
  protected:
    static void register_cleanup() {}

  public:
    static SNMALLOC_FAST_PATH Alloc* get_noncachable()
    {
      return (Alloc*)ThreadAllocUntyped::get();
    }

    static SNMALLOC_FAST_PATH Alloc* get()
    {
      return (Alloc*)ThreadAllocUntyped::get();
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

  using ThreadAlloc = ThreadAllocUntypedWrapper;
#else

  /**
   * Common aspects of thread local allocator. Subclasses handle how releasing
   * the allocator is triggered.
   */
  class ThreadAllocCommon
  {
    friend void register_clean_up();

  protected:
    static inline void inner_release()
    {
      get()->teardown();
    }

    /**
     * Default clean up does nothing except print statistics if enabled.
     */
    static bool register_cleanup()
    {
#  ifdef USE_SNMALLOC_STATS
      Singleton<int, atexit_print_stats>::get();
#  endif
      return false;
    }

#  ifdef USE_SNMALLOC_STATS
    static void print_stats()
    {
      Stats s;
      current_alloc_pool()->aggregate_stats(s);
      s.print<Alloc>(std::cout);
    }

    static int atexit_print_stats() noexcept
    {
      return atexit(print_stats);
    }
#  endif

  public:
    /**
     * TODO: new API
     * Public interface, returns the allocator for this thread, constructing
     * one if necessary.
     *
     * If no operations have been performed on an allocator returned by either
     * `get()` nor `get_noncachable()`, then the value contained in the return
     * will be an Alloc that will always use the slow path.
     *
     * Only use this API if you intend to use the returned allocator just once
     * per call, or if you know other calls have already been made to the
     * allocator.
     */
    static inline Alloc* get_noncachable()
    {
      SNMALLOC_REQUIRE_CONSTINIT static thread_local Alloc alloc;
      return &alloc;
    }

    /**
     * TODO API
     * Public interface, returns the allocator for this thread, constructing
     * one if necessary.
     * This incurs a cost, so use `get_noncachable` if you can meet its
     * criteria.
     */
    static SNMALLOC_FAST_PATH Alloc* get()
    {
      return get_noncachable();
    }
  };

  /**
   * Version of the `ThreadAlloc` interface that uses a hook provided by libc
   * to destroy thread-local state.  This is the ideal option, because it
   * enforces ordering of destruction such that the malloc state is destroyed
   * after anything that can allocate memory.
   *
   * This class is used only when snmalloc is compiled as part of a compatible
   * libc (for example, FreeBSD libc).
   */
  class ThreadAllocLibcCleanup : public ThreadAllocCommon
  {
    /**
     * Libc will call `_malloc_thread_cleanup` just before a thread terminates.
     * This function must be allowed to call back into this class to destroy
     * the state.
     */
    friend void ::_malloc_thread_cleanup();
  };

  /**
   * Version of the `ThreadAlloc` interface that uses C++ `thread_local`
   * destructors for cleanup.  If a per-thread allocator is used during the
   * destruction of other per-thread data, this class will create a new
   * instance and register its destructor, so should eventually result in
   * cleanup, but may result in allocators being returned to the global pool
   * and then reacquired multiple times.
   *
   * This implementation depends on nothing outside of a working C++
   * environment and so should be the simplest for initial bringup on an
   * unsupported platform.  It is currently used in the FreeBSD kernel version.
   */
  class ThreadAllocThreadDestructor : public ThreadAllocCommon
  {
    template<void f()>
    friend class OnDestruct;

  public:
    static pthread_key_t register_cleanup1() noexcept
    {
      pthread_key_t key;
      pthread_key_create(&key, &register_cleanup2);
      return key;
    }
    static void register_cleanup2(void*)
    {
      inner_release();
    }

    static void register_cleanup()
    {
      static Singleton<pthread_key_t, &register_cleanup1> key;
      
      // Need non-null field for destructor to be called.
      pthread_setspecific(key.get(), (void*)1);

      ThreadAllocCommon::register_cleanup();
    }
  };

#  ifdef SNMALLOC_USE_THREAD_CLEANUP
  /**
   * Entry point that allows libc to call into the allocator for per-thread
   * cleanup.
   */
  extern "C" void _malloc_thread_cleanup()
  {
    ThreadAllocLibcCleanup::inner_release();
  }
  using ThreadAlloc = ThreadAllocLibcCleanup;
#  else
  using ThreadAlloc = ThreadAllocThreadDestructor;
#  endif

  /**
   * TODO revise comment (probably all in this file.)
   * Function passed as a template parameter to `Allocator` to allow lazy
   * replacement. This function returns true, if the allocated passed in,
   * is the placeholder allocator.  If it returns true, then
   * `init_thread_allocator` should be called.
   */
  inline SNMALLOC_FAST_PATH void register_clean_up()
  {
    ThreadAlloc::register_cleanup();
  }
#endif
} // namespace snmalloc
