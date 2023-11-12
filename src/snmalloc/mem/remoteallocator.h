#pragma once

#include "freelist_queue.h"
#include "remotecache.h"

namespace snmalloc
{
  /**
   * A RemoteAllocator is the message queue of freed objects.  It builds on the
   * FreeListMPSCQ but encapsulates knowledge that the objects are actually
   * RemoteMessage-s and not just any freelist::object::T<>s.
   *
   * RemoteAllocator-s may be exposed to client tampering.  As a result,
   * pointer domestication may be necessary.  See the documentation for
   * FreeListMPSCQ for details.
   */
  struct RemoteAllocator
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

    FreeListMPSCQ<key_global> list;

    using alloc_id_t = address_t;

    constexpr RemoteAllocator() = default;

    void invariant()
    {
      list.invariant();
    }

    void init()
    {
      list.init();
    }

    freelist::QueuePtr destroy()
    {
      return list.destroy();
    }

    template<typename Domesticator_head, typename Domesticator_queue>
    inline bool can_dequeue(
      Domesticator_head domesticate_head, Domesticator_queue domesticate_queue)
    {
      return list.can_dequeue(domesticate_head, domesticate_queue);
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
      list.enqueue(first, last, domesticate_head);
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
      list.dequeue(domesticate_head, domesticate_queue, cb);
    }

    alloc_id_t trunc_id()
    {
      return address_cast(this);
    }
  };
} // namespace snmalloc
