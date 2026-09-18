#include <snmalloc/snmalloc.h>
#include <test/setup.h>

using namespace snmalloc;

namespace
{
  FreeListKey queue_key{0x1234, 0x5678, 0x9abc};
  using Queue = FreeListMPSCQ<queue_key>;
  using Object = freelist::Object::T<>;

  freelist::HeadPtr domesticate(freelist::QueuePtr p)
  {
    return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
  }

  freelist::HeadPtr as_head(Object& object)
  {
    return freelist::HeadPtr::unsafe_from(&object);
  }

  template<typename T, size_t Size>
  struct CallbackOrder
  {
    T values[Size];
    size_t count = 0;

    void add(T value)
    {
      SNMALLOC_CHECK(count < Size);
      values[count++] = value;
    }
  };

  /**
   * An empty drain invokes no callback.  An empty dequeue domesticates the null
   * head, but applies neither the queue domesticator nor the callback.
   */
  void test_empty()
  {
    Queue queue;
    size_t head_domesticates = 0;
    size_t queue_domesticates = 0;
    size_t callbacks = 0;

    queue.drain_and_reset(
      domesticate, domesticate, [&callbacks](freelist::HeadPtr) {
        callbacks++;
      });
    SNMALLOC_CHECK(callbacks == 0);

    auto domesticate_head =
      [&head_domesticates](freelist::QueuePtr value) -> freelist::HeadPtr {
      head_domesticates++;
      SNMALLOC_CHECK(value == nullptr);
      return nullptr;
    };
    auto domesticate_queue =
      [&queue_domesticates](freelist::QueuePtr) -> freelist::HeadPtr {
      queue_domesticates++;
      return nullptr;
    };

    queue.dequeue(
      domesticate_head, domesticate_queue, [&callbacks](freelist::HeadPtr) {
        callbacks++;
        return true;
      });

    SNMALLOC_CHECK(head_domesticates == 1);
    SNMALLOC_CHECK(queue_domesticates == 0);
    SNMALLOC_CHECK(callbacks == 0);
  }

  /**
   * Enqueue reports whether it starts a queue generation and preserves FIFO
   * order for single-element and pre-linked multi-element enqueues.  After a
   * callback stops dequeue, the next dequeue resumes at the successor.
   * Dequeue retains the object at back; drain delivers it and resets the queue
   * for reuse.
   */
  void test_queue_contract()
  {
    Queue queue;
    Object first;
    Object second;
    Object batch_first;
    Object batch_last;
    Object replacement;
    CallbackOrder<freelist::HeadPtr, 5> order;

    SNMALLOC_CHECK(queue.enqueue(as_head(first), as_head(first), domesticate));
    SNMALLOC_CHECK(
      !queue.enqueue(as_head(second), as_head(second), domesticate));

    freelist::Object::atomic_store_next(
      as_head(batch_first), as_head(batch_last), queue_key, NO_KEY_TWEAK);
    SNMALLOC_CHECK(
      !queue.enqueue(as_head(batch_first), as_head(batch_last), domesticate));

    queue.dequeue(domesticate, domesticate, [&order](freelist::HeadPtr value) {
      order.add(value);
      return false;
    });
    SNMALLOC_CHECK(order.count == 1);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(as_head(first)));

    queue.dequeue(domesticate, domesticate, [&order](freelist::HeadPtr value) {
      order.add(value);
      return true;
    });
    SNMALLOC_CHECK(order.count == 3);
    SNMALLOC_CHECK(
      address_cast(order.values[1]) == address_cast(as_head(second)));
    SNMALLOC_CHECK(
      address_cast(order.values[2]) == address_cast(as_head(batch_first)));

    queue.drain_and_reset(
      domesticate, domesticate, [&order](freelist::HeadPtr value) {
        order.add(value);
      });
    SNMALLOC_CHECK(order.count == 4);
    SNMALLOC_CHECK(
      address_cast(order.values[3]) == address_cast(as_head(batch_last)));

    SNMALLOC_CHECK(
      queue.enqueue(as_head(replacement), as_head(replacement), domesticate));
    queue.drain_and_reset(
      domesticate, domesticate, [&order](freelist::HeadPtr value) {
        order.add(value);
      });
    SNMALLOC_CHECK(order.count == 5);
    SNMALLOC_CHECK(
      address_cast(order.values[4]) == address_cast(as_head(replacement)));
  }

  /**
   * The captured back bounds a dequeue before any successor link is read.  A
   * link beyond that back belongs to an enqueue this dequeue does not cover, so
   * it is neither domesticated nor followed.  The subsequent drain confirms
   * that the retained object remains queued.
   */
  void test_dequeue_checks_bound_first()
  {
    Queue queue;
    Object tail;
    Object outside;
    size_t queue_domesticates = 0;
    size_t dequeue_callbacks = 0;
    size_t drain_callbacks = 0;

    SNMALLOC_CHECK(queue.enqueue(as_head(tail), as_head(tail), domesticate));
    freelist::Object::atomic_store_next(
      as_head(tail), as_head(outside), queue_key, NO_KEY_TWEAK);

    auto counting_domesticate =
      [&queue_domesticates](freelist::QueuePtr value) -> freelist::HeadPtr {
      queue_domesticates++;
      return domesticate(value);
    };

    queue.dequeue(
      domesticate,
      counting_domesticate,
      [&dequeue_callbacks](freelist::HeadPtr) {
        dequeue_callbacks++;
        return true;
      });

    SNMALLOC_CHECK(queue_domesticates == 0);
    SNMALLOC_CHECK(dequeue_callbacks == 0);

    queue.drain_and_reset(
      domesticate, domesticate, [&](freelist::HeadPtr value) {
        SNMALLOC_CHECK(address_cast(value) == address_cast(as_head(tail)));
        drain_callbacks++;
      });
    SNMALLOC_CHECK(drain_callbacks == 1);
  }

  /**
   * Drain resets the queue before invoking callbacks.  An enqueue from a
   * callback therefore starts a replacement chain that is not consumed until
   * the next drain.
   */
  void test_callback_starts_replacement()
  {
    Queue queue;
    Object old_first;
    Object old_last;
    Object replacement_first;
    Object replacement_last;
    CallbackOrder<freelist::HeadPtr, 4> order;
    bool replacement_started = false;

    freelist::Object::atomic_store_next(
      as_head(old_first), as_head(old_last), queue_key, NO_KEY_TWEAK);
    SNMALLOC_CHECK(
      queue.enqueue(as_head(old_first), as_head(old_last), domesticate));

    queue.drain_and_reset(
      domesticate, domesticate, [&](freelist::HeadPtr value) {
        order.add(value);
        if (address_cast(value) == address_cast(as_head(old_first)))
        {
          freelist::Object::atomic_store_next(
            as_head(replacement_first),
            as_head(replacement_last),
            queue_key,
            NO_KEY_TWEAK);
          replacement_started = queue.enqueue(
            as_head(replacement_first), as_head(replacement_last), domesticate);
        }
      });

    SNMALLOC_CHECK(replacement_started);
    SNMALLOC_CHECK(order.count == 2);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(as_head(old_first)));
    SNMALLOC_CHECK(
      address_cast(order.values[1]) == address_cast(as_head(old_last)));

    queue.drain_and_reset(
      domesticate, domesticate, [&order](freelist::HeadPtr value) {
        order.add(value);
      });
    SNMALLOC_CHECK(order.count == 4);
    SNMALLOC_CHECK(
      address_cast(order.values[2]) ==
      address_cast(as_head(replacement_first)));
    SNMALLOC_CHECK(
      address_cast(order.values[3]) == address_cast(as_head(replacement_last)));
  }

  /**
   * RemoteAllocator::drain_and_reset uses domesticate_head for the value read
   * from front and domesticate_queue for successors read from message links.
   * It recovers each RemoteMessage from its link, which requires a non-zero
   * displacement when remote messages are batched.
   */
  void test_remote_allocator_uses_distinct_domesticators()
  {
    RemoteAllocator remote;
    RemoteMessage first{};
    RemoteMessage second{};
    auto first_message = capptr::Alloc<RemoteMessage>::unsafe_from(&first);
    auto second_message = capptr::Alloc<RemoteMessage>::unsafe_from(&second);
    auto first_link = RemoteMessage::to_message_link(first_message);
    auto second_link = RemoteMessage::to_message_link(second_message);
    size_t head_domesticates = 0;
    size_t queue_domesticates = 0;
    CallbackOrder<capptr::Alloc<RemoteMessage>, 2> order;

    SNMALLOC_CHECK(remote.enqueue(first_message, first_message, domesticate));
    SNMALLOC_CHECK(
      !remote.enqueue(second_message, second_message, domesticate));

    auto domesticate_head = [&](freelist::QueuePtr value) -> freelist::HeadPtr {
      head_domesticates++;
      SNMALLOC_CHECK(address_cast(value) == address_cast(first_link));
      return domesticate(value);
    };
    auto domesticate_queue =
      [&](freelist::QueuePtr value) -> freelist::HeadPtr {
      queue_domesticates++;
      SNMALLOC_CHECK(address_cast(value) == address_cast(second_link));
      return domesticate(value);
    };

    remote.drain_and_reset(
      domesticate_head,
      domesticate_queue,
      [&order](capptr::Alloc<RemoteMessage> value) { order.add(value); });

    SNMALLOC_CHECK(head_domesticates == 1);
    SNMALLOC_CHECK(queue_domesticates == 1);
    SNMALLOC_CHECK(order.count == 2);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(first_message));
    SNMALLOC_CHECK(
      address_cast(order.values[1]) == address_cast(second_message));
  }
}

int main()
{
  setup();
  test_empty();
  test_queue_contract();
  test_dequeue_checks_bound_first();
  test_callback_starts_replacement();
  test_remote_allocator_uses_distinct_domesticators();
}
