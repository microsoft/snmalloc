#pragma once

#include "bits.h"

#include <atomic>

namespace snmalloc
{
  /**
   * @brief The DebugFlagWord struct
   * Wrapper for std::atomic_flag so that we can examine
   * the re-entrancy problem at debug mode.
   */
  struct DebugFlagWord
  {
    /**
     * @brief flag
     * The underlying atomic field.
     */
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    constexpr DebugFlagWord() = default;

    template<typename... Args>
    constexpr DebugFlagWord(Args&&... args) : flag(std::forward<Args>(args)...)
    {}

    /**
     * @brief set_owner
     * Record the identity of the locker.
     */
    void set_owner()
    {
      SNMALLOC_ASSERT(nullptr == owner);
      owner = get_thread_identity();
    }

    /**
     * @brief clear_owner
     * Set the identity to null.
     */
    void clear_owner()
    {
      SNMALLOC_ASSERT(get_thread_identity() == owner);
      owner = nullptr;
    }

    /**
     * @brief assert_not_owned_by_current_thread
     * Assert the lock should not be held already by current thread.
     */
    void assert_not_owned_by_current_thread()
    {
      SNMALLOC_ASSERT(get_thread_identity() != owner);
    }

  private:
    using ThreadIdentity = int const*;

    /**
     * @brief owner
     * We use a pointer to TLS field as the thread identity.
     * std::thread::id can be another solution but it does not
     * support `constexpr` initialisation on some platforms.
     */
    ThreadIdentity owner = nullptr;

    /**
     * @brief get_thread_identity
     * @return The identity of current thread.
     */
    inline ThreadIdentity get_thread_identity()
    {
      static thread_local int SNMALLOC_THREAD_IDENTITY = 0;
      return &SNMALLOC_THREAD_IDENTITY;
    }
  };

  /**
   * @brief The ReleaseFlagWord struct
   * The shares the same structure with DebugFlagWord but
   * all member functions associated with ownership checkings
   * are empty so that they can be optimised out at Release mode.
   */
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
#ifdef __cpp_lib_atomic_flag_test
      do
      {
        // test once with acquire ordering at the beginning
        if (!lock.flag.test_and_set(std::memory_order_acquire))
        {
          break;
        }
        // assert_not_owned_by_current_thread is only called when the first
        // acquiring is failed; which means the lock is already held somewhere
        // else.
        lock.assert_not_owned_by_current_thread();
        // only need to do relaxed ordering in the busy spin loop
        while (lock.flag.test(std::memory_order_relaxed))
        {
          Aal::pause();
        }
      } while (true);
#else
      while (lock.flag.test_and_set(std::memory_order_acquire))
      {
        lock.assert_not_owned_by_current_thread();
        Aal::pause();
      }
#endif
      lock.set_owner();
    }

    ~FlagLock()
    {
      lock.clear_owner();
      lock.flag.clear(std::memory_order_release);
    }
  };
} // namespace snmalloc
