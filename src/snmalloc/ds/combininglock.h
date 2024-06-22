#pragma once

#include "../aal/aal.h"
#include "../pal/pal.h"

#include <atomic>
#include <functional>

namespace snmalloc
{
  class CombineLockNode;
  using CombiningLock = std::atomic<CombineLockNode*>;

  /**
   * @brief Combinations of MCS queue lock with Flat Combining
   *
   * Each element in the queue has a pointer to a work item.
   * This means when under contention the thread holding the lock
   * can perform the work.
   *
   * As the work items are arbitrary lambdas there are no simplifications
   * for combining related work items.  I.e. original Flat Combining paper
   * might sort a collection of inserts, and perform them in a single traversal.
   *
   * Note that, we should perhaps add a Futex/WakeOnAddress mode to improve
   * performance in the contended case, rather than spinning.
   */
  class CombineLockNode
  {
    enum class LockStatus
    {
      // The work for this node has not been completed.
      WAITING,

      // The work for this thread has been completed, and it is not the
      // last element in the queue.
      DONE,

      // The work for this thread has not been completed, and it is the
      // head of the queue.
      READY
    };

    // Status of the queue, set by the thread at the head of the queue,
    // When it makes the thread for this node either the head of the queue
    // or completes its work.
    std::atomic<LockStatus> status{LockStatus::WAITING};

    // Used to store the queue
    std::atomic<CombineLockNode*> next{nullptr};

    // Stores the C++ lambda associated with this node in the queue.
    void (*f_raw)(CombineLockNode*);

    void set_status(LockStatus s)
    {
      status.store(s, std::memory_order_release);
    }

  public:
    constexpr CombineLockNode(void (*f)(CombineLockNode*)) : f_raw(f) {}

    void attach(CombiningLock& lock)
    {
      // Add to the queue of pending work
      auto prev = lock.exchange(this, std::memory_order_acq_rel);

      if (prev != nullptr)
      {
        // If we aren't the head, link into predecessor
        prev->next.store(this, std::memory_order_release);

        // Wait to for predecessor to complete
        while (status.load(std::memory_order_relaxed) == LockStatus::WAITING)
          Aal::pause();

        // Determine if another thread completed our work.
        if (status.load(std::memory_order_acquire) == LockStatus::DONE)
          return;
      }
      // We could add an else branch here that could set
      //    status = LockStatus::Ready
      // However, the subsequent state assumes it is Ready, and
      // nothing would read it.

      // We are the head of the queue, and responsible for
      // waking/performing our and subsequent work.
      auto curr = this;
      while (true)
      {
        // Perform work for head of the queue
        curr->f_raw(curr);

        // Determine if there are more elements.
        auto n = curr->next.load(std::memory_order_acquire);
        if (n != nullptr)
        {
          // Signal this work was completed and move on to
          // next item.
          curr->set_status(LockStatus::DONE);
          curr = n;
          continue;
        }

        // This could be the end of the queue, attempt to close the
        // queue.
        auto curr_c = curr;
        if (lock.compare_exchange_strong(curr_c, nullptr, std::memory_order_acq_rel))
        {
          // Queue was successfully closed.
          // Notify last element the work was completed.
          curr->set_status(LockStatus::DONE);
          return;
        }

        // Failed to close the queue wait for next thread to be
        // added.
        while (curr->next.load(std::memory_order_relaxed) == nullptr)
          Aal::pause();

        // As we had to wait, give the job to the next thread
        // to carry on performing the work.
        n = curr->next.load(std::memory_order_acquire);
        n->set_status(LockStatus::READY);

        // Notify the thread that we completed its work.
        // Note that this needs to be done last, as we can't read
        // curr->next after setting curr->status
        curr->set_status(LockStatus::DONE);
        return;
      }
    }
  };

  template<typename F>
  class CombineLockNodeTempl : CombineLockNode
  {
    template<typename FF>
    friend void with(CombiningLock&, FF&&);

    // This holds the closure for the lambda
    F f;

    // Untyped version of calling f to store in the node.
    static void invoke(CombineLockNode* self)
    {
      auto self_templ = reinterpret_cast<CombineLockNodeTempl*>(self);
      self_templ->f();
    }

    CombineLockNodeTempl(CombiningLock& lock, F&& f_)
    : CombineLockNode(invoke), f(f_)
    {
      attach(lock);
    }
  };

  /**
   * Lock primitive. This takes a reference to a Lock, and a thunk to
   * call when the lock is available.  The thunk should be independent of
   * the current thread as the thunk may be executed by a different thread.
   */
  template<typename F>
  inline void with(CombiningLock& lock, F&& f)
  {
    CombineLockNodeTempl<F> node{lock, std::forward<F>(f)};
  }
} // namespace snmalloc
