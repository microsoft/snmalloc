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
   * Function passed as a tempalte parameter to `Allocator` to allow lazy
   * replacement.  There is nothing to initialise in this case, so we expect
   * this to never be called.
   */
  SNMALLOC_FAST_PATH void* init_thread_allocator()
  {
    return nullptr;
  }

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
    auto a = reinterpret_cast<const Alloc*>(&GlobalPlaceHolder);
    return const_cast<Alloc*>(a);
  }

  /**
   * Common aspects of thread local allocator. Subclasses handle how releasing
   * the allocator is triggered.
   */
  class ThreadAllocCommon
  {
    friend void* init_thread_allocator();

  protected:
    static inline void inner_release()
    {
      auto& per_thread = get_reference();
      if (per_thread != get_GlobalPlaceHolder())
      {
        current_alloc_pool()->release(per_thread);
        per_thread = get_GlobalPlaceHolder();
      }
    }

    /**
     * Default clean up does nothing except print statistics if enabled.
     */
    static void register_cleanup()
    {
#  ifdef USE_SNMALLOC_STATS
      Singleton<int, atexit_print_stats>::get();
#  endif
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
      static thread_local Alloc* alloc = get_GlobalPlaceHolder();
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
     *
     * The returned Alloc* is guaranteed to be initialised.  This incurs a cost,
     * so use `get_noncachable` if you can meet its criteria.
     */
    static SNMALLOC_FAST_PATH Alloc* get()
    {
#  ifdef USE_MALLOC
      return get_reference();
#  else
      auto alloc = get_reference();
      if (unlikely(needs_initialisation(alloc)))
      {
        alloc = reinterpret_cast<Alloc*>(init_thread_allocator());
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
    static void register_cleanup()
    {
      static thread_local OnDestruct<ThreadAllocCommon::inner_release> tidier;

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
   * Slow path for the placeholder replacement.
   * Function passed as a tempalte parameter to `Allocator` to allow lazy
   * replacement.  This function initialises the thread local state if requried.
   * The simple check that this is the global placeholder is inlined, the rest
   * of it is only hit in a very unusual case and so should go off the fast
   * path.
   */
  SNMALLOC_SLOW_PATH inline void* init_thread_allocator()
  {
    auto*& local_alloc = ThreadAlloc::get_reference();
    if (local_alloc != get_GlobalPlaceHolder())
    {
      // If someone reuses a noncachable call, then we can end up here.
      // The allocator has already been initialised. Could either error
      // to say stop doing this, or just give them the initialised version.
      return local_alloc;
    }
    local_alloc = current_alloc_pool()->acquire();
    SNMALLOC_ASSERT(local_alloc != get_GlobalPlaceHolder());
    ThreadAlloc::register_cleanup();
    return local_alloc;
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
