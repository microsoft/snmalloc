#pragma once

#include "../aal/aal.h"
#include "../pal/pal.h"

#include <atomic>
#include <functional>

namespace snmalloc
{
  /**
   * @brief The DebugFlagWord struct
   * Wrapper for std::atomic_flag so that we can examine
   * the re-entrancy problem at debug mode.
   */
  struct DebugFlagWord
  {
    using ThreadIdentity = DefaultPal::ThreadIdentity;

    /**
     * @brief flag
     * The underlying atomic field.
     */
    std::atomic_bool flag{false};

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
      SNMALLOC_ASSERT(ThreadIdentity() == owner);
      owner = get_thread_identity();
    }

    /**
     * @brief clear_owner
     * Set the identity to null.
     */
    void clear_owner()
    {
      SNMALLOC_ASSERT(get_thread_identity() == owner);
      owner = ThreadIdentity();
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
    /**
     * @brief owner
     * We use the Pal to provide the ThreadIdentity.
     */
    std::atomic<ThreadIdentity> owner = ThreadIdentity();

    /**
     * @brief get_thread_identity
     * @return The identity of current thread.
     */
    static ThreadIdentity get_thread_identity()
    {
      return DefaultPal::get_tid();
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
    std::atomic_bool flag{false};

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
      while (lock.flag.exchange(true, std::memory_order_acquire))
      {
        // assert_not_owned_by_current_thread is only called when the first
        // acquiring is failed; which means the lock is already held somewhere
        // else.
        lock.assert_not_owned_by_current_thread();
        // This loop is better for spin-waiting because it won't issue
        // expensive write operation (xchg for example).
        while (lock.flag.load(std::memory_order_relaxed))
        {
          Aal::pause();
        }
      }
      lock.set_owner();
    }

    ~FlagLock()
    {
      lock.clear_owner();
      lock.flag.store(false, std::memory_order_release);
    }
  };
} // namespace snmalloc
