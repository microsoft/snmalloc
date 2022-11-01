#pragma once

#include "../aal/aal.h"
#include "../pal/pal.h"

#include <atomic>
#include <functional>

namespace snmalloc
{
  /**
   * This file implements the classic MCS Queue Lock
   *
   * This does not require any allocation and can thus be
   * used very early in snmalloc.
   *
   * Mellor-Crummey, J.M., Scott, M.L.: Algorithms for scalable synchronization
   * on shared-memory multiprocessors.
   * https://www.cs.rochester.edu/~scott/papers/1991_TOCS_synch.pdf
   */

  struct WaitNode
  {
    std::atomic<bool> flag{false};
    std::atomic<WaitNode*> next{nullptr};

    constexpr WaitNode() = default;

    /**
     * Remove all the move and copy operations. This
     * structure is address taken, and used in the queue
     * of waiters. It is not safe to move or copy it.
     */
    WaitNode(const WaitNode&) = delete;
    WaitNode& operator=(const WaitNode&) = delete;
    WaitNode(WaitNode&&) = delete;
    WaitNode& operator=(WaitNode&&) = delete;
  };

  /**
   * @brief The DebugFlagWord struct
   * Wrapper for std::atomic_flag so that we can examine
   * the re-entrancy problem at debug mode.
   */
  struct DebugFlagWord
  {
    using ThreadIdentity = DefaultPal::ThreadIdentity;

    /**
     * @brief waiters
     * The underlying atomic field containing linked list of
     * waiting threads' nodes.
     */
    std::atomic<WaitNode*> waiters{nullptr};

    constexpr DebugFlagWord() = default;

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
    std::atomic<WaitNode*> waiters{nullptr};

    constexpr ReleaseFlagWord() = default;

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
    WaitNode node{};
    FlagWord& lock;

  public:
    FlagLock(FlagWord& lock) : lock(lock)
    {
      auto prev = lock.waiters.exchange(&node, std::memory_order_acquire);
      if (prev != nullptr)
      {
        prev->next.store(&node, std::memory_order_release);
        while (!(node.flag.load(std::memory_order_acquire)))
          Aal::pause();
      }
      lock.set_owner();
    }

    ~FlagLock()
    {
      lock.clear_owner();
      if (node.next.load(std::memory_order_relaxed) == nullptr)
      {
        auto expected = &node;
        if (lock.waiters.compare_exchange_strong(
              expected, nullptr, std::memory_order_release))
          return;
        while (node.next.load(std::memory_order_acquire) == nullptr)
          Aal::pause();
      }
      node.next.load(std::memory_order_relaxed)
        ->flag.store(true, std::memory_order_release);
    }
  };
} // namespace snmalloc
