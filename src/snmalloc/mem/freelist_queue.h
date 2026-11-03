#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "snmalloc/stl/atomic.h"

namespace snmalloc
{
  enum class EnqueueResult
  {
    StartedActive,
    StartedInactive,
    Appended
  };

  /**
   * A FreeListMPSCQ is a chain of freed objects exposed as a MPSC append-only
   * atomic queue that uses one xchg per append.
   *
   * The internal pointers are considered QueuePtr-s to support deployment
   * scenarios in which the MPSCQ itself is exposed to the client.  This is
   * excessively paranoid in the common case that these metadata are as "hard"
   * for the client to reach as the Pagemap, which we trust to store not just
   * Tame CapPtr<>s but raw C++ pointers.
   *
   * Where necessary, dequeue and draining expose two domesticator callbacks
   * and are careful to use one for the front value and the other for pointers
   * read from the queue itself.  Specifically,
   *
   *   * `domesticate_head` is used for the MPSCQ pointers used to reach into
   *     the chain of objects
   *
   *   * `domesticate_queue` is used to traverse links in that chain.
   *
   * In the case that the MPSCQ is not easily accessible to the client,
   * `domesticate_head` can just be a type coersion, and `domesticate_queue`
   * should perform actual validation.  If the MPSCQ is exposed to the
   * allocator client, both Domesticators should perform validation.
   */
  template<FreeListKey& Key, address_t Key_tweak = NO_KEY_TWEAK>
  struct alignas(REMOTE_MIN_ALIGN) FreeListMPSCQ
  {
    // Store the message queue on a separate cacheline. It is mutable data that
    // is read by other threads.
    alignas(CACHELINE_SIZE) freelist::AtomicQueuePtr back{nullptr};
    // Store the two ends on different cache lines as access by different
    // threads.
    alignas(CACHELINE_SIZE) freelist::AtomicQueuePtr front{nullptr};

    constexpr FreeListMPSCQ() = default;

    void invariant()
    {
      SNMALLOC_ASSERT(pointer_align_up(this, REMOTE_MIN_ALIGN) == this);
    }

  private:
    template<typename Domesticator_queue, typename Cb>
    freelist::HeadPtr process_chain(
      freelist::HeadPtr curr,
      freelist::QueuePtr target,
      Domesticator_queue& domesticate,
      Cb& cb)
    {
      // Read the successor before invoking the callback.  If the callback
      // rejects curr, curr remains owned by the queue with a published
      // successor.
      while (address_cast(curr) != address_cast(target))
      {
        auto next = curr->atomic_read_next(Key, Key_tweak, domesticate);
        if (SNMALLOC_UNLIKELY(next == nullptr))
          return curr;

        Aal::prefetch(next.unsafe_ptr());
        if (SNMALLOC_UNLIKELY(!cb(curr)))
          return curr;

        curr = next;
      }

      // The returned node is unprocessed and remains in the queue.
      return curr;
    }

  public:
    static freelist::QueuePtr inactive_marker()
    {
      return freelist::QueuePtr::unsafe_from(
        unsafe_from_uintptr<freelist::Object::T<>>(1));
    }

    bool release_to_inactive()
    {
      freelist::QueuePtr expected = nullptr;
      if (back.compare_exchange_strong(
            expected,
            inactive_marker(),
            stl::memory_order_acq_rel,
            stl::memory_order_acquire))
      {
        return false;
      }

      SNMALLOC_ASSERT(expected != inactive_marker());
      return true;
    }

    bool claim_from_inactive()
    {
      auto expected = back.load(stl::memory_order_acquire);
      if (expected == inactive_marker())
      {
        if (back.compare_exchange_strong(
              expected,
              nullptr,
              stl::memory_order_acq_rel,
              stl::memory_order_acquire))
        {
          return false;
        }
      }

      SNMALLOC_ASSERT(expected != nullptr);
      SNMALLOC_ASSERT(expected != inactive_marker());
      return expected != nullptr;
    }

    void assert_not_inactive()
    {
      SNMALLOC_ASSERT(
        back.load(stl::memory_order_relaxed) != inactive_marker());
    }

    bool is_empty()
    {
      auto value = back.load(stl::memory_order_acquire);
      return value == nullptr || value == inactive_marker();
    }

    /**
     * Exactly one consumer role may execute this operation.  No dequeue,
     * owning-allocator queue processing, or second drain may run concurrently.
     *
     * A producer that has exchanged back must eventually publish front or its
     * predecessor link; otherwise this operation waits indefinitely.
     *
     * domesticate_head applies only to values loaded from front.
     * domesticate_queue applies to successors decoded from message links.
     *
     * The queue is reset before the first callback.  The callback may therefore
     * release or re-enqueue an object; any re-enqueue belongs to the
     * replacement chain and is not consumed by this invocation.
     */
    template<
      typename Domesticator_head,
      typename Domesticator_queue,
      typename Cb>
    void drain_and_reset(
      Domesticator_head domesticate_head,
      Domesticator_queue domesticate_queue,
      Cb cb)
    {
      assert_not_inactive();

      // After reuse, acquire the release sequence headed by the preceding
      // reset, so front cannot observe an earlier queue generation.
      if (back.load(stl::memory_order_acquire) == nullptr)
        return;

      freelist::HeadPtr curr = nullptr;
      do
      {
        auto raw = front.load(stl::memory_order_acquire);
        if (raw != nullptr)
          curr = domesticate_head(raw);
        if (curr == nullptr)
          Aal::pause();
      } while (curr == nullptr);

      // A producer that observes null back may immediately publish a new front,
      // so the old front must be cleared before resetting back.
      front.store(nullptr, stl::memory_order_relaxed);
      auto target = back.exchange(nullptr, stl::memory_order_acq_rel);
      SNMALLOC_ASSERT(target != nullptr);
      SNMALLOC_ASSERT(target != inactive_marker());

      auto process = [&cb](freelist::HeadPtr p) {
        cb(p);
        return true;
      };

      while (true)
      {
        curr = process_chain(curr, target, domesticate_queue, process);
        if (address_cast(curr) == address_cast(target))
          break;
        Aal::pause();
      }
      cb(curr);
    }

    inline bool can_dequeue()
    {
      assert_not_inactive();
      return front.load(stl::memory_order_relaxed) !=
        back.load(stl::memory_order_relaxed);
    }

    /**
     * Pushes a list of messages to the queue. Each message from first to
     * last should be linked together through their next pointers.
     *
     * The Domesticator here is used only on pointers read from the head.  See
     * the commentary on the class.
     *
     * Reports whether this enqueue started an active or inactive queue
     * generation, or appended to an existing chain.
     */
    template<typename Domesticator_head>
    EnqueueResult enqueue(
      freelist::HeadPtr first,
      freelist::HeadPtr last,
      Domesticator_head domesticate_head)
    {
      invariant();
      freelist::Object::atomic_store_null(last, Key, Key_tweak);

      // // The following non-linearisable effect is normally benign,
      // // but could lead to a remote list become completely detached
      // // during a fork in a multi-threaded process. This would lead
      // // to a memory leak, which is probably the least of your problems
      // // if you forked in during a deallocation.  We can prevent this
      // // with the following code, but it is not currently enabled as it
      // // has negative performance impact.
      // // An alternative would be to reset the queue on the child postfork
      // // handler to ensure that the queue has not been blackholed.
      // PreventFork pf;
      // snmalloc::UNUSED(pf);

      // Exchange needs to be acq_rel.
      // *  It needs to be a release, so nullptr in next is visible.
      // *  Needs to be acquire, so linking into the list does not race with
      //    the other threads nullptr init of the next field.
      freelist::QueuePtr prev =
        back.exchange(capptr_rewild(last), stl::memory_order_acq_rel);

      if (SNMALLOC_UNLIKELY(prev == nullptr || prev == inactive_marker()))
      {
        // drain_and_reset clears front before resetting back, so only a
        // producer whose exchange observed an empty state may publish a
        // replacement front.
        front.store(capptr_rewild(first));
        return prev == nullptr ? EnqueueResult::StartedActive :
                                 EnqueueResult::StartedInactive;
      }

      // Once this store publishes first, a drain may observe it and release
      // prev; this must therefore be this producer's final access to prev.
      freelist::Object::atomic_store_next(
        domesticate_head(prev), first, Key, Key_tweak);
      return EnqueueResult::Appended;
    }

    /**
     * Destructively iterate the queue.  Each queue element is removed and fed
     * to the callback in turn.  The callback may return false to reject its
     * argument and stop iteration early.  A rejected object must not have been
     * consumed, freed, or re-enqueued; it remains owned by this queue.
     *
     * Closing dequeue requires the callback to process the retained final
     * object after the queue has been closed.
     *
     * Takes a domestication callback for each of "pointers read from head" and
     * "pointers read from queue".  See the commentary on the class.
     */
    template<
      bool Close = false,
      typename Domesticator_head,
      typename Domesticator_queue,
      typename Cb>
    void dequeue(
      Domesticator_head domesticate_head,
      Domesticator_queue domesticate_queue,
      Cb cb)
    {
      invariant();

      freelist::HeadPtr curr = domesticate_head(front.load());
      if (curr == nullptr)
      {
        // First entry is still in progress of being added.
        // Nothing to do.
        return;
      }

      // Use back to bound, so we don't handle new entries.
      auto b = back.load(stl::memory_order_relaxed);

      /*
       * process_chain may return a pointer domesticated from a queue link.
       * Publishing it to client-accessible front requires it to be considered
       * Wild again in !QueueHeadsAreTame builds.
       */
      if constexpr (Close)
      {
        curr = process_chain(curr, b, domesticate_queue, cb);

        auto current_back = back.load(stl::memory_order_acquire);
        if (address_cast(curr) == address_cast(current_back))
        {
          front.store(nullptr, stl::memory_order_relaxed);
          if (back.compare_exchange_strong(
                current_back,
                nullptr,
                stl::memory_order_acq_rel,
                stl::memory_order_acquire))
          {
            // A producer that could still access curr would have changed back,
            // making the compare-exchange fail.
            SNMALLOC_CHECK(cb(curr));
            return;
          }
        }

        front.store(capptr_rewild(curr), stl::memory_order_release);
      }
      else
      {
        curr = process_chain(curr, b, domesticate_queue, cb);
        front = capptr_rewild(curr);
      }
      invariant();
    }
  };
} // namespace snmalloc
