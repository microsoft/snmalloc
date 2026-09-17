#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "snmalloc/stl/atomic.h"

namespace snmalloc
{
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
   * Where necessary, dequeue exposes two domesticator callbacks and is careful
   * to use one for the front value and the other for pointers read from the
   * queue itself.  Draining uses its queue domesticator for both the front
   * value and links in the chain.  Specifically,
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
      while (address_cast(curr) != address_cast(target))
      {
        auto next = curr->atomic_read_next(Key, Key_tweak, domesticate);
        if (SNMALLOC_UNLIKELY(next == nullptr))
          return curr;

        Aal::prefetch(next.unsafe_ptr());
        if (SNMALLOC_UNLIKELY(!cb(curr)))
          return next;

        curr = next;
      }

      return curr;
    }

  public:
    /**
     * Exactly one consumer role may execute this operation.  No dequeue,
     * owning-allocator queue processing, or second drain may run concurrently.
     *
     * A producer that has exchanged back must eventually publish front or its
     * predecessor link; otherwise this operation waits indefinitely.
     *
     * The queue is reset before the first callback.  The callback may therefore
     * release or re-enqueue an object; any re-enqueue belongs to the
     * replacement chain and is not consumed by this invocation.
     */
    template<typename Domesticator_queue, typename Cb>
    void drain_and_reset(Domesticator_queue domesticate, Cb cb)
    {
      // After reuse, acquire the release sequence headed by the preceding
      // reset, so front cannot observe an earlier queue generation.
      if (back.load(stl::memory_order_acquire) == nullptr)
        return;

      freelist::HeadPtr curr = nullptr;
      do
      {
        auto raw = front.load(stl::memory_order_acquire);
        if (raw != nullptr)
          curr = domesticate(raw);
        if (curr == nullptr)
          Aal::pause();
      } while (curr == nullptr);

      // A producer that observes null back may immediately publish a new front,
      // so the old front must be cleared before resetting back.
      front.store(nullptr, stl::memory_order_relaxed);
      auto target = back.exchange(nullptr, stl::memory_order_acq_rel);
      SNMALLOC_ASSERT(target != nullptr);

      auto process = [&cb](freelist::HeadPtr p) {
        cb(p);
        return true;
      };

      while (true)
      {
        curr = process_chain(curr, target, domesticate, process);
        if (address_cast(curr) == address_cast(target))
          break;
        Aal::pause();
      }
      cb(curr);
    }

    inline bool can_dequeue()
    {
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
     * Returns true if this enqueue observed an empty back and started a new
     * queue generation by publishing front.  Returns false if it appended to
     * an existing chain.
     */
    template<typename Domesticator_head>
    bool enqueue(
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

      if (SNMALLOC_LIKELY(prev != nullptr))
      {
        // Once this store publishes first, a drain may observe it and release
        // prev; this must therefore be this producer's final access to prev.
        freelist::Object::atomic_store_next(
          domesticate_head(prev), first, Key, Key_tweak);
        return false;
      }

      // drain_and_reset clears front before resetting back, so only a producer
      // whose exchange observed null may publish a replacement front.
      front.store(capptr_rewild(first));
      return true;
    }

    /**
     * Destructively iterate the queue.  Each queue element is removed and fed
     * to the callback in turn.  The callback may return false to stop iteration
     * early (but must have processed the element it was given!).
     *
     * Takes a domestication callback for each of "pointers read from head" and
     * "pointers read from queue".  See the commentary on the class.
     */
    template<
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
      curr = process_chain(curr, b, domesticate_queue, cb);
      front = capptr_rewild(curr);
      invariant();
    }
  };
} // namespace snmalloc
