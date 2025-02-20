#pragma once

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
  public:
    static SNMALLOC_FAST_PATH Alloc& get()
    {
      return ThreadAllocExternal::get();
    }

    // This will always call the success path as the client is responsible
    // handling the initialisation.
    using CheckInit = CheckInitDefault;
  };

#else

  class CheckInitPthread;
  class CheckInitCXX;

  /**
   * Holds the thread local state for the allocator.  The state is constant
   * initialised, and has no direct dectructor.  Instead snmalloc will call
   * `register_clean_up` on the slow path for bringing up thread local state.
   * This is responsible for calling `teardown`, which effectively destructs the
   * data structure, but in a way that allow it to still be used.
   */
  class ThreadAlloc
  {
    SNMALLOC_REQUIRE_CONSTINIT static inline thread_local Alloc alloc{};

    // As allocation and deallocation can occur during thread teardown
    // we need to record if we are already in that state as we will not
    // receive another teardown call, so each operation needs to release
    // the underlying data structures after the call.
    static inline thread_local bool teardown_called{false};

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
      return alloc;
    }

    template<typename Subclass>
    class CheckInitBase
    {
      template<typename Restart, typename... Args>
      SNMALLOC_SLOW_PATH static auto check_init_slow(Restart r, Args... args)
      {
        bool post_teardown = teardown_called;

        if constexpr (!Config::Options.LocalAllocSupportsLazyInit)
        {
          SNMALLOC_CHECK(
            false &&
            "lazy_init called on an allocator that doesn't support lazy "
            "initialisation");
          // Unreachable, but needed to keep the type checker happy in deducing
          // the return type of this function.
          return static_cast<decltype(action(args...))>(nullptr);
        }
        else
        {
          // Initialise the thread local allocator
          if constexpr (Config::Options.CoreAllocOwnsLocalState)
          {
            alloc.init();
          }

          // register_clean_up must be called after init.  register clean up
          // may be implemented with allocation, so need to ensure we have a
          // valid allocator at this point.
          if (!post_teardown)
          {
            // Must be called at least once per thread.
            // A pthread implementation only calls the thread destruction handle
            // if the key has been set.
            Subclass::register_clean_up();

            // Perform underlying operation
            return r(args...);
          }

          OnDestruct od([]() {
#  ifdef SNMALLOC_TRACING
            message<1024>("post_teardown flush()");
#  endif
            // We didn't have an allocator because the thread is being torndown.
            // We need to return any local state, so we don't leak it.
            alloc.teardown();
          });

          // Perform underlying operation
          return r(args...);
        }
      }

    public:
      template<typename Success, typename Restart, typename... Args>
      SNMALLOC_FAST_PATH static auto
      check_init(Success s, Restart r, Args... args)
      {
        if (alloc.is_init())
        {
          return s();
        }

        return check_init_slow(r, args...);
      }

      static void teardown()
      {
        teardown_called = true;
        alloc.teardown();
      }
    };
#  ifdef SNMALLOC_USE_PTHREAD_DESTRUCTORS
    using CheckInit = CheckInitPthread;
#  elif defined(SNMALLOC_USE_CXX_THREAD_DESTRUCTORS)
    using CheckInit = CheckInitCXX;
#  else
    using CheckInit = CheckInitDefault;
#  endif
  };

#  ifdef SNMALLOC_USE_PTHREAD_DESTRUCTORS
  class CheckInitPthread : public ThreadAlloc::CheckInitBase<CheckInitPthread>
  {
  private:
    /**
     * Used to give correct signature to teardown required by pthread_key.
     */
    static void pthread_cleanup(void*)
    {
      teardown();
    }

    /**
     * Used to give correct signature to teardown required by atexit.
     */
    static void pthread_cleanup_main_thread()
    {
      teardown();
    }

    /**
     * Used to give correct signature to the pthread call for the Singleton
     * class.
     */
    static void pthread_create(pthread_key_t* key) noexcept
    {
      pthread_key_create(key, &pthread_cleanup);
      // Main thread does not call pthread_cleanup if `main` returns or `exit`
      // is called, so use an atexit handler to guarantee that the cleanup is
      // run at least once.  If the main thread exits with `pthread_exit` then
      // it will be called twice but this case is already handled because other
      // destructors can cause the per-thread allocator to be recreated.
      atexit(&pthread_cleanup_main_thread);
    }

  public:
    /**
     * Performs thread local teardown for the allocator using the pthread
     * library.
     *
     * This removes the dependence on the C++ runtime.
     */
    static void register_clean_up()
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
  };
#  elif defined(SNMALLOC_USE_CXX_THREAD_DESTRUCTORS)
  class CheckInitCXX : public ThreadAlloc::CheckInitBase<CheckInitCXX>
  {
  public:
    /**
     * This function is called by each thread once it starts using the
     * thread local allocator.
     *
     * This implementation depends on nothing outside of a working C++
     * environment and so should be the simplest for initial bringup on an
     * unsupported platform.
     */
    static void register_clean_up()
    {
      static thread_local OnDestruct dummy([]() { teardown(); });
      UNUSED(dummy);
#    ifdef SNMALLOC_TRACING
      message<1024>("Using C++ destructor clean up");
#    endif
    }
  };
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
#endif
