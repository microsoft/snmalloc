#pragma once

#include "bits.h"

namespace snmalloc
{
  class FlagLock
  {
  private:
    std::atomic_flag& lock;

  public:
    FlagLock(std::atomic_flag& lock) : lock(lock)
    {
      while (lock.test_and_set(std::memory_order_acquire))
        bits::pause();
    }

    ~FlagLock()
    {
      lock.clear(std::memory_order_release);
    }
  };
} // namespace snmalloc
