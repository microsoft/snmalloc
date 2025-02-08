#pragma once

#include <snmalloc/aal/aal.h>
#include <snmalloc/stl/atomic.h>
#include <stddef.h>

#ifdef SNMALLOC_PTHREAD_ATFORK_WORKS
#  include <pthread.h>

namespace snmalloc
{
  // This is a simple implementation of a class that can be
  // used to prevent a process from forking. Holding a lock
  // in the allocator while forking can lead to deadlocks.
  // This causes the fork to wait out any other threads inside
  // the allocators locks.
  //
  // The use is
  // ```
  //  {
  //     PreventFork pf;
  //     // Code that should not be running during a fork.
  //  }
  // ```
  class PreventFork
  {
    // Global atomic counter of the number of threads currently preventing the
    // system from forking. The bottom bit is used to signal that a thread is
    // wanting to fork.
    static inline stl::Atomic<size_t> threads_preventing_fork{0};

    // The depth of the current thread's prevention of forking.
    // This is used to enable reentrant prevention of forking.
    static inline thread_local size_t depth_of_prevention{0};

    // There could be multiple copies of the atfork handler installed.
    // Only perform work for the first prefork and final postfork.
    static inline thread_local size_t depth_of_handlers{0};

    // The function that notifies new threads not to enter PreventFork regions
    // It waits until all threads are no longer in a PreventFork region before
    // returning.
    static void prefork()
    {
      if (depth_of_handlers++ != 0)
        return;

      if (depth_of_prevention != 0)
        error("Fork attempted while in PreventFork region.");

      while (true)
      {
        auto current = threads_preventing_fork.load();
        if (
          (current % 2 == 0) &&
          (threads_preventing_fork.compare_exchange_weak(current, current + 1)))
        {
          break;
        }
        Aal::pause();
      };

      while (threads_preventing_fork.load() != 1)
      {
        Aal::pause();
      }

      // Finally set the flag that allows this thread to enter PreventFork
      // regions This is safe as the only other calls here are to other prefork
      // handlers.
      depth_of_prevention++;
    }

    // Unsets the flag that allows threads to enter PreventFork regions
    // and for another thread to request a fork.
    static void postfork()
    {
      if (--depth_of_handlers != 0)
        return;

      // This thread is no longer preventing a fork, so decrement the counter.
      depth_of_prevention--;

      // Allow other threads to allocate
      threads_preventing_fork = 0;
    }

    // This function ensures that the fork handler has been installed at least once.
    // It might be installed more than once, this is safe. As subsequent calls would
    // be ignored.
    static void ensure_init()
    {
      static stl::Atomic<bool> initialised{false};

      if (initialised.load(std::memory_order_acquire))
        return;

      pthread_atfork(prefork, postfork, postfork);
      initialised.store(true, std::memory_order_release);
    };

  public:
    PreventFork()
    {
      if (depth_of_prevention++ == 0)
      {
        // Ensure that the system is initialised before we start.
        // Don't do this on nested Prevent calls.
        ensure_init();
        while (true)
        {
          auto current = threads_preventing_fork.load();

          if (
            (current % 2 == 0) &&
            threads_preventing_fork.compare_exchange_weak(current, current + 2))
          {
            break;
          }
          Aal::pause();
        };
      }
    }

    ~PreventFork()
    {
      if (--depth_of_prevention == 0)
      {
        threads_preventing_fork -= 2;
      }
    }
  };
} // namespace snmalloc
#else
namespace snmalloc
{
  class PreventFork
  {};
} // namespace snmalloc
#endif