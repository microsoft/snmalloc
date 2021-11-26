#pragma once

#include "bits.h"

#include <atomic>

#ifndef NDEBUG
#  include <thread>
#endif

namespace snmalloc
{
  class FlagLock
  {
  private:
    std::atomic_flag& lock;

#ifndef NDEBUG
    std::thread::id owner{};
#endif
  public:
    FlagLock(std::atomic_flag& lock) : lock(lock)
    {
      while (lock.test_and_set(std::memory_order_acquire))
      {
#ifndef NDEBUG
        SNMALLOC_ASSERT(std::this_thread::get_id() != owner);
#endif
        Aal::pause();
      }
#ifndef NDEBUG
      owner = std::this_thread::get_id();
#endif
    }

    ~FlagLock()
    {
      lock.clear(std::memory_order_release);
#ifndef NDEBUG
      owner = {};
#endif
    }
  };
} // namespace snmalloc
