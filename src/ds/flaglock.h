#pragma once

#include "bits.h"

#include <atomic>

namespace snmalloc
{
  struct DebugFlagWord
  {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    constexpr DebugFlagWord() = default;

    template<typename... Args>
    constexpr DebugFlagWord(Args&&... args) : flag(std::forward<Args>(args)...)
    {}

    void set_owner()
    {
      SNMALLOC_ASSERT(nullptr == owner);
      owner = get_thread_identity();
    }

    void clear_owner()
    {
      SNMALLOC_ASSERT(get_thread_identity() == owner);
      owner = nullptr;
    }

    void assert_not_owned_by_current_thread()
    {
      SNMALLOC_ASSERT(get_thread_identity() != owner);
    }

  private:
    using ThreadIdentity = int const*;

    ThreadIdentity owner = nullptr;

    inline ThreadIdentity get_thread_identity()
    {
      static thread_local int SNMALLOC_THREAD_IDENTITY = 0;
      return &SNMALLOC_THREAD_IDENTITY;
    }
  };

  struct ReleaseFlagWord
  {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    constexpr ReleaseFlagWord() = default;

    template<typename... Args>
    constexpr ReleaseFlagWord(Args&&... args)
    : flag(std::forward<Args>(args)...)
    {}

    void set_owner() {}
    void clear_owner() {}
    void assert_not_owned_by_current_thread() {}
  };

#ifdef NDEBUG
  using FlagWord = ReleaseFlagWord;
#else
  using FlagWord = DebugFlagWord;
#endif

  class FlagLock
  {
  private:
    FlagWord& lock;

  public:
    FlagLock(FlagWord& lock) : lock(lock)
    {
      while (lock.flag.test_and_set(std::memory_order_acquire))
      {
        lock.assert_not_owned_by_current_thread();
        Aal::pause();
      }
      lock.set_owner();
    }

    ~FlagLock()
    {
      lock.clear_owner();
      lock.flag.clear(std::memory_order_release);
    }
  };
} // namespace snmalloc
