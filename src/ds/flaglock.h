#pragma once

#include "bits.h"

#include <atomic>
#include <thread>

namespace snmalloc
{
  struct DebugFlagWord
  {
    std::atomic_flag flag{};
    std::thread::id owner{};
    constexpr DebugFlagWord() = default;
    template<typename... Args>
    constexpr DebugFlagWord(Args&&... args) : flag(std::forward<Args>(args)...)
    {}
    void set_owner()
    {
      owner = std::this_thread::get_id();
    }
    void clear_owner()
    {
      owner = {};
    }
    void assert_not_owned_by_current_thread()
    {
      SNMALLOC_ASSERT(std::this_thread::get_id() != owner);
    }
  };
  struct ReleaseFlagWord
  {
    std::atomic_flag flag{};
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
