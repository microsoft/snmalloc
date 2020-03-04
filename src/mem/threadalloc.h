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
   * replacement.  In this case we are assuming the underlying external thread
   * alloc is performing initialization, so this is not required, and just
   * always returns nullptr to specify no new allocator is required.
   */
  SNMALLOC_FAST_PATH void* lazy_replacement(void* existing)
  {
    UNUSED(existing);
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
   */
  inline GlobalVirtual dummy_memory_provider;
  inline Alloc GlobalPlaceHolder(
    dummy_memory_provider, SNMALLOC_DEFAULT_CHUNKMAP(), nullptr, true);

  /**
   * Common aspects of thread local allocator. Subclasses handle how releasing
   * the allocator is triggered.
   */
  class ThreadAllocCommon
  {
    friend void* lazy_replacement_slow();

  protected:
    static inline void inner_release()
    {
      auto& per_thread = get_reference();
      if (per_thread != &GlobalPlaceHolder)
      {
        current_alloc_pool()->release(per_thread);
        per_thread = &GlobalPlaceHolder;
      }
    }

    /**
     * Default clean up does nothing except print statistics if enabled.
     **/
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
      static thread_local Alloc* alloc = &GlobalPlaceHolder;
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
      auto new_alloc = lazy_replacement(alloc);
      return (likely(new_alloc == nullptr)) ?
        alloc :
        reinterpret_cast<Alloc*>(new_alloc);
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
   * Slow path for the placeholder replacement.  The simple check that this is
   * the global placeholder is inlined, the rest of it is only hit in a very
   * unusual case and so should go off the fast path.
   */
  SNMALLOC_SLOW_PATH inline void* lazy_replacement_slow()
  {
    auto*& local_alloc = ThreadAlloc::get_reference();
    SNMALLOC_ASSERT(local_alloc == &GlobalPlaceHolder);
    local_alloc = current_alloc_pool()->acquire();
    SNMALLOC_ASSERT(local_alloc != &GlobalPlaceHolder);
    ThreadAlloc::register_cleanup();
    return local_alloc;
  }

  /**
   * Function passed as a template parameter to `Allocator` to allow lazy
   * replacement.  This is called on all of the slow paths in `Allocator`.  If
   * the caller is the global placeholder allocator then this function will
   * check if we've already allocated a per-thread allocator, returning it if
   * so.  If we have not allocated a per-thread allocator yet, then this
   * function will allocate one.
   */
  SNMALLOC_FAST_PATH void* lazy_replacement(void* existing)
  {
    if (existing != &GlobalPlaceHolder)
    {
      return nullptr;
    }
    return lazy_replacement_slow();
  }
#endif
} // namespace snmalloc
