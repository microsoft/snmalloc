#pragma once

#include "../ds/ds.h"
#include "freelist.h"
#include "metadata.h"
#include "sizeclasstable.h"

#include <array>
#include <atomic>

namespace snmalloc
{
  /**
   *
   * A RemoteAllocator is the message queue of freed objects.  It exposes a MPSC
   * append-only atomic queue that uses one xchg per append.
   *
   * The internal pointers are considered QueuePtr-s to support deployment
   * scenarios in which the RemoteAllocator itself is exposed to the client.
   * This is excessively paranoid in the common case that the RemoteAllocator-s
   * are as "hard" for the client to reach as the Pagemap, which we trust to
   * store not just Tame CapPtr<>s but raw C++ pointers.
   *
   * While we could try to condition the types used here on a flag in the
   * backend's `struct Flags Options` value, we instead expose two domesticator
   * callbacks at the interface and are careful to use one for the front and
   * back values and the other for pointers read from the queue itself.  That's
   * not ideal, but it lets the client condition its behavior appropriately and
   * prevents us from accidentally following either of these pointers in generic
   * code.
   *
   * `domesticate_head` is used for the pointer used to reach the of the queue,
   * while `domesticate_queue` is used to traverse the first link in the queue
   * itself.  In the case that the RemoteAllocator is not easily accessible to
   * the client, `domesticate_head` can just be a type coersion, and
   * `domesticate_queue` should perform actual validation.  If the
   * RemoteAllocator is exposed to the client, both Domesticators should perform
   * validation.
   */
  struct alignas(REMOTE_MIN_ALIGN) RemoteAllocator
  {
    /**
     * Global key for all remote lists.
     *
     * Note that we use a single key for all remote free lists and queues.
     * This is so that we do not have to recode next pointers when sending
     * segments, and look up specific keys based on destination.  This is
     * potentially more performant, but could make it easier to guess the key.
     */
    inline static FreeListKey key_global{0xdeadbeef, 0xbeefdead, 0xdeadbeef};

    using alloc_id_t = address_t;

    // Store the message queue on a separate cacheline. It is mutable data that
    // is read by other threads.
    alignas(CACHELINE_SIZE) freelist::AtomicQueuePtr back{nullptr};
    // Store the two ends on different cache lines as access by different
    // threads.
    alignas(CACHELINE_SIZE) freelist::AtomicQueuePtr front{nullptr};
    // Fake first entry
    freelist::Object::T<capptr::bounds::AllocWild> stub{};

    constexpr RemoteAllocator() = default;

    void invariant()
    {
      SNMALLOC_ASSERT(
        (address_cast(front.load()) == address_cast(&stub)) ||
        (back != nullptr));
    }

    void init()
    {
      freelist::HeadPtr stub_ptr = freelist::HeadPtr::unsafe_from(&stub);
      freelist::Object::atomic_store_null(stub_ptr, key_global);
      front.store(freelist::QueuePtr::unsafe_from(&stub));
      back.store(nullptr, std::memory_order_relaxed);
      invariant();
    }

    freelist::QueuePtr destroy()
    {
      freelist::QueuePtr fnt = front.load();
      back.store(nullptr, std::memory_order_relaxed);
      if (address_cast(front.load()) == address_cast(&stub))
        return nullptr;
      return fnt;
    }

    template<typename Domesticator_head, typename Domesticator_queue>
    inline bool can_dequeue(
      Domesticator_head domesticate_head, Domesticator_queue domesticate_queue)
    {
      return domesticate_head(front.load())
               ->atomic_read_next(key_global, domesticate_queue) != nullptr;
    }

    /**
     * Pushes a list of messages to the queue. Each message from first to
     * last should be linked together through their next pointers.
     *
     * The Domesticator here is used only on pointers read from the head.  See
     * the commentary on the class.
     */
    template<typename Domesticator_head>
    void enqueue(
      freelist::HeadPtr first,
      freelist::HeadPtr last,
      Domesticator_head domesticate_head)
    {
      invariant();
      freelist::Object::atomic_store_null(last, key_global);

      // Exchange needs to be acq_rel.
      // *  It needs to be a release, so nullptr in next is visible.
      // *  Needs to be acquire, so linking into the list does not race with
      //    the other threads nullptr init of the next field.
      freelist::QueuePtr prev =
        back.exchange(capptr_rewild(last), std::memory_order_acq_rel);

      if (SNMALLOC_LIKELY(prev != nullptr))
      {
        freelist::Object::atomic_store_next(
          domesticate_head(prev), first, key_global);
        return;
      }

      front.store(capptr_rewild(first));
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
      SNMALLOC_ASSERT(front.load() != nullptr);

      // Use back to bound, so we don't handle new entries.
      auto b = back.load(std::memory_order_relaxed);
      freelist::HeadPtr curr = domesticate_head(front.load());

      while (address_cast(curr) != address_cast(b))
      {
        freelist::HeadPtr next =
          curr->atomic_read_next(key_global, domesticate_queue);
        // We have observed a non-linearisable effect of the queue.
        // Just go back to allocating normally.
        if (SNMALLOC_UNLIKELY(next == nullptr))
          break;
        // We want this element next, so start it loading.
        Aal::prefetch(next.unsafe_ptr());
        if (SNMALLOC_UNLIKELY(!cb(curr)))
        {
          /*
           * We've domesticate_queue-d next so that we can read through it, but
           * we're storing it back into client-accessible memory in
           * !QueueHeadsAreTame builds, so go ahead and consider it Wild again.
           * On QueueHeadsAreTame builds, the subsequent domesticate_head call
           * above will also be a type-level sleight of hand, but we can still
           * justify it by the domesticate_queue that happened in this
           * dequeue().
           */
          front = capptr_rewild(next);
          invariant();
          return;
        }

        curr = next;
      }

      /*
       * Here, we've hit the end of the queue: next is nullptr and curr has not
       * been handed to the callback.  The same considerations about Wildness
       * above hold here.
       */
      front = capptr_rewild(curr);
      invariant();
    }

    alloc_id_t trunc_id()
    {
      return address_cast(this);
    }
  };
} // namespace snmalloc
