#pragma once

#include "../ds/helpers.h"
#include "globalalloc.h"
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
   * replacement. This function returns true, if the allocator passed in
   * requires initialisation. As the TLS state is managed externally,
   * this will always return false.
   */
  SNMALLOC_FAST_PATH bool needs_initialisation(void* existing)
  {
    UNUSED(existing);
    return false;
  }

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
  SNMALLOC_FAST_PATH void* init_thread_allocator(function_ref<void*(void*)> f)
  {
    error("Critical Error: This should never be called.");
    return f(nullptr);
  }
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif

  using ThreadAlloc = ThreadAllocUntypedWrapper;
#else
  /**
   * A global fake allocator object.  This never allocates memory and, as a
   * result, never owns any slabs.  On the slow paths, where it would fetch
   * slabs to allocate from, it will discover that it is the placeholder and
   * replace itself with the thread-local allocator, allocating one if
   * required.  This avoids a branch on the fast path.
   *
   * The fake allocator is a zero initialised area of memory of the correct
   * size. All data structures used potentially before initialisation must be
   * okay with zero init to move to the slow path, that is, zero must signify
   * empty.
   */
  inline const char GlobalPlaceHolder[sizeof(Alloc)] = {0};
  inline Alloc* get_GlobalPlaceHolder()
  {
    // This cast is not legal.  Effectively, we want a minimal constructor
    // for the global allocator as zero, and then a second constructor for
    // the rest.  This is UB.
    auto a = reinterpret_cast<const Alloc*>(&GlobalPlaceHolder);
    return const_cast<Alloc*>(a);
  }

  /**
   * Common aspects of thread local allocator. Subclasses handle how releasing
   * the allocator is triggered.
   */
  class ThreadAllocCommon
  {
    friend void* init_thread_allocator(function_ref<void*(void*)>);

  protected:
    /**
     * Thread local variable that is set to true, once `inner_release`
     * has been run.  If we try to reinitialise the allocator once
     * `inner_release` has run, then we can stay on the slow path so we don't
     * leak allocators.
     *
     * This is required to allow for the allocator to be called during
     * destructors of other thread_local state.
     */
    inline static thread_local bool destructor_has_run = false;

    static inline void inner_release()
    {
      auto& per_thread = get_reference();
      if (per_thread != get_GlobalPlaceHolder())
      {
        current_alloc_pool()->release(per_thread);
        destructor_has_run = true;
        per_thread = get_GlobalPlaceHolder();
      }
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
     * Returns a reference to the allocator for the current thread. This allows
     * the caller to replace the current thread's allocator.
     */
    static inline Alloc*& get_reference()
    {
      // Inline casting as codegen doesn't create a lazy init like this.
      static thread_local Alloc* alloc =
        const_cast<Alloc*>(reinterpret_cast<const Alloc*>(&GlobalPlaceHolder));
      return alloc;
    }

    /**
     * Public interface, returns the allocator for this thread, constructing
     * one if necessary.
     *
     * If no operations have been performed on an allocator returned by either
     * `get()` nor `get_noncachable()`, then the value contained in the return
     * will be an Alloc* that will always use the slow path.
     *
     * Only use this API if you intend to use the returned allocator just once
     * per call, or if you know other calls have already been made to the
     * allocator.
     */
    static inline Alloc* get_noncachable()
    {
      return get_reference();
    }

    /**
     * Public interface, returns the allocator for this thread, constructing
     * one if necessary.
     * This incurs a cost, so use `get_noncachable` if you can meet its
     * criteria.
     */
    static SNMALLOC_FAST_PATH Alloc* get()
    {
#  ifdef SNMALLOC_PASS_THROUGH
      return get_reference();
#  else
      auto*& alloc = get_reference();
      if (unlikely(needs_initialisation(alloc)) && !destructor_has_run)
      {
        // Call `init_thread_allocator` to perform down call in case
        // register_clean_up does more.
        // During teardown for the destructor based ThreadAlloc this will set
        // alloc to GlobalPlaceHolder;
        init_thread_allocator([](void*) { return nullptr; });
      }
      return alloc;
#  endif
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
    static bool register_cleanup()
    {
      static thread_local OnDestruct<ThreadAllocCommon::inner_release> tidier;

      ThreadAllocCommon::register_cleanup();

      return destructor_has_run;
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
   * Slow path for the placeholder replacement.
   * Function passed as a tempalte parameter to `Allocator` to allow lazy
   * replacement.  This function initialises the thread local state if requried.
   * The simple check that this is the global placeholder is inlined, the rest
   * of it is only hit in a very unusual case and so should go off the fast
   * path.
   * The second component of the return indicates if this TLS is being torndown.
   */
  SNMALLOC_FAST_PATH void* init_thread_allocator(function_ref<void*(void*)> f)
  {
    auto*& local_alloc = ThreadAlloc::get_reference();
    // If someone reuses a noncachable call, then we can end up here
    // with an already initialised allocator. Could either error
    // to say stop doing this, or just give them the initialised version.
    if (local_alloc == get_GlobalPlaceHolder())
    {
      local_alloc = current_alloc_pool()->acquire();
    }
    auto result = f(local_alloc);
    // Check if we have already run the destructor for the TLS.  If so,
    // we need to deallocate the allocator.
    if (ThreadAlloc::register_cleanup())
      ThreadAlloc::inner_release();
    return result;
  }

  /**
   * Function passed as a template parameter to `Allocator` to allow lazy
   * replacement. This function returns true, if the allocated passed in,
   * is the placeholder allocator.  If it returns true, then
   * `init_thread_allocator` should be called.
   */
  SNMALLOC_FAST_PATH bool needs_initialisation(void* existing)
  {
    return existing == get_GlobalPlaceHolder();
  }
#endif
} // namespace snmalloc
