#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>
#include <thread>
#include <vector>

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

  template<typename Predicate>
  void wait_until(Predicate predicate)
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate())
    {
      SNMALLOC_CHECK(std::chrono::steady_clock::now() < deadline);
      std::this_thread::yield();
    }
  }

  struct CallbackOrder
  {
    freelist::HeadPtr values[8];
    size_t count = 0;

    void add(freelist::HeadPtr value)
    {
      values[count++] = value;
    }
  };

  void check_empty(Queue& queue)
  {
    SNMALLOC_CHECK(queue.back.load(stl::memory_order_relaxed) == nullptr);
    SNMALLOC_CHECK(queue.front.load(stl::memory_order_relaxed) == nullptr);
  }

  void test_empty()
  {
    Queue queue;
    size_t callbacks = 0;

    queue.drain_and_reset(
      domesticate, [&callbacks](freelist::HeadPtr) { callbacks++; });

    SNMALLOC_CHECK(callbacks == 0);
    check_empty(queue);
  }

  void test_enqueue_and_iterate()
  {
    Queue queue;
    Object first;
    Object second;
    Object batch_first;
    Object batch_last;
    Object replacement;
    CallbackOrder order;

    SNMALLOC_CHECK(queue.enqueue(as_head(first), as_head(first), domesticate));
    SNMALLOC_CHECK(
      !queue.enqueue(as_head(second), as_head(second), domesticate));

    freelist::Object::atomic_store_next(
      as_head(batch_first), as_head(batch_last), queue_key, NO_KEY_TWEAK);
    SNMALLOC_CHECK(
      !queue.enqueue(as_head(batch_first), as_head(batch_last), domesticate));

    queue.drain_and_reset(
      domesticate, [&order](freelist::HeadPtr value) { order.add(value); });

    SNMALLOC_CHECK(order.count == 4);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(as_head(first)));
    SNMALLOC_CHECK(
      address_cast(order.values[1]) == address_cast(as_head(second)));
    SNMALLOC_CHECK(
      address_cast(order.values[2]) == address_cast(as_head(batch_first)));
    SNMALLOC_CHECK(
      address_cast(order.values[3]) == address_cast(as_head(batch_last)));
    check_empty(queue);

    SNMALLOC_CHECK(
      queue.enqueue(as_head(replacement), as_head(replacement), domesticate));
    queue.enqueue(as_head(second), as_head(second), domesticate);

    order.count = 0;
    queue.drain_and_reset(
      domesticate, [&order](freelist::HeadPtr value) { order.add(value); });
    SNMALLOC_CHECK(order.count == 2);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(as_head(replacement)));
    SNMALLOC_CHECK(
      address_cast(order.values[1]) == address_cast(as_head(second)));
    check_empty(queue);
  }

  void test_dequeue_empty()
  {
    Queue queue;
    Object pending;
    size_t callbacks = 0;
    auto cb = [&callbacks](freelist::HeadPtr) {
      callbacks++;
      return true;
    };

    queue.dequeue(domesticate, domesticate, cb);
    SNMALLOC_CHECK(callbacks == 0);
    check_empty(queue);

    freelist::Object::atomic_store_null(
      as_head(pending), queue_key, NO_KEY_TWEAK);
    auto prev = queue.back.exchange(
      capptr_rewild(as_head(pending)), stl::memory_order_acq_rel);
    SNMALLOC_CHECK(prev == nullptr);

    queue.dequeue(domesticate, domesticate, cb);
    SNMALLOC_CHECK(callbacks == 0);
    SNMALLOC_CHECK(queue.front.load(stl::memory_order_relaxed) == nullptr);
    SNMALLOC_CHECK(
      address_cast(queue.back.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(pending)));

    queue.back.store(nullptr, stl::memory_order_relaxed);
    check_empty(queue);
  }

  void test_dequeue_positions()
  {
    Queue queue;
    Object objects[3];
    CallbackOrder order;

    freelist::Object::atomic_store_next(
      as_head(objects[0]), as_head(objects[1]), queue_key, NO_KEY_TWEAK);
    freelist::Object::atomic_store_next(
      as_head(objects[1]), as_head(objects[2]), queue_key, NO_KEY_TWEAK);
    SNMALLOC_CHECK(
      queue.enqueue(as_head(objects[0]), as_head(objects[2]), domesticate));

    queue.dequeue(domesticate, domesticate, [&order](freelist::HeadPtr value) {
      order.add(value);
      return false;
    });

    SNMALLOC_CHECK(order.count == 1);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(as_head(objects[0])));
    SNMALLOC_CHECK(
      address_cast(queue.front.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(objects[1])));

    queue.dequeue(domesticate, domesticate, [&order](freelist::HeadPtr value) {
      order.add(value);
      return true;
    });

    SNMALLOC_CHECK(order.count == 2);
    SNMALLOC_CHECK(
      address_cast(order.values[1]) == address_cast(as_head(objects[1])));
    SNMALLOC_CHECK(
      address_cast(queue.front.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(objects[2])));
    SNMALLOC_CHECK(
      address_cast(queue.back.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(objects[2])));

    order.count = 0;
    queue.drain_and_reset(
      domesticate, [&order](freelist::HeadPtr value) { order.add(value); });
    SNMALLOC_CHECK(order.count == 1);
    SNMALLOC_CHECK(
      address_cast(order.values[0]) == address_cast(as_head(objects[2])));
    check_empty(queue);
  }

  void test_dequeue_publication_gap()
  {
    Queue queue;
    Object first;
    Object second;
    size_t callbacks = 0;

    SNMALLOC_CHECK(queue.enqueue(as_head(first), as_head(first), domesticate));

    freelist::Object::atomic_store_null(
      as_head(second), queue_key, NO_KEY_TWEAK);
    auto prev = queue.back.exchange(
      capptr_rewild(as_head(second)), stl::memory_order_acq_rel);
    SNMALLOC_CHECK(address_cast(prev) == address_cast(as_head(first)));

    queue.dequeue(domesticate, domesticate, [&callbacks](freelist::HeadPtr) {
      callbacks++;
      return true;
    });

    SNMALLOC_CHECK(callbacks == 0);
    SNMALLOC_CHECK(
      address_cast(queue.front.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(first)));

    freelist::Object::atomic_store_next(
      as_head(first), as_head(second), queue_key, NO_KEY_TWEAK);
    queue.drain_and_reset(
      domesticate, [&callbacks](freelist::HeadPtr) { callbacks++; });
    SNMALLOC_CHECK(callbacks == 2);
    check_empty(queue);
  }

  void test_dequeue_checks_bound_first()
  {
    Queue queue;
    Object first;
    Object outside;
    size_t callbacks = 0;
    size_t domesticates = 0;

    SNMALLOC_CHECK(queue.enqueue(as_head(first), as_head(first), domesticate));
    freelist::Object::atomic_store_next(
      as_head(first), as_head(outside), queue_key, NO_KEY_TWEAK);

    auto counting_domesticate =
      [&domesticates](freelist::QueuePtr value) -> freelist::HeadPtr {
      domesticates++;
      return domesticate(value);
    };

    queue.dequeue(
      domesticate, counting_domesticate, [&callbacks](freelist::HeadPtr) {
        callbacks++;
        return true;
      });

    SNMALLOC_CHECK(callbacks == 0);
    SNMALLOC_CHECK(domesticates == 0);
    SNMALLOC_CHECK(
      address_cast(queue.front.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(first)));
    SNMALLOC_CHECK(
      address_cast(queue.back.load(stl::memory_order_relaxed)) ==
      address_cast(as_head(first)));

    freelist::Object::atomic_store_null(
      as_head(first), queue_key, NO_KEY_TWEAK);
    queue.drain_and_reset(domesticate, [](freelist::HeadPtr) {});
    check_empty(queue);
  }

  void test_remote_allocator_move_only_callback()
  {
    RemoteAllocator remote;
    RemoteMessage message{};
    size_t callbacks = 0;
    auto message_ptr = capptr::Alloc<RemoteMessage>::unsafe_from(&message);

    SNMALLOC_CHECK(remote.enqueue(message_ptr, message_ptr, domesticate));

    auto cb = [token = std::make_unique<size_t>(1),
               &callbacks](capptr::Alloc<RemoteMessage>) mutable {
      callbacks += (*token)++;
    };
    remote.drain_and_reset(domesticate, std::move(cb));

    SNMALLOC_CHECK(callbacks == 1);
    SNMALLOC_CHECK(remote.list.back.load(stl::memory_order_relaxed) == nullptr);
    SNMALLOC_CHECK(
      remote.list.front.load(stl::memory_order_relaxed) == nullptr);
  }

  void test_front_publication_gap()
  {
    Queue queue;
    Object object;
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
    std::atomic<size_t> callbacks{0};

    freelist::Object::atomic_store_null(
      as_head(object), queue_key, NO_KEY_TWEAK);
    auto prev = queue.back.exchange(
      capptr_rewild(as_head(object)), stl::memory_order_acq_rel);
    SNMALLOC_CHECK(prev == nullptr);

    std::thread consumer([&]() {
      entered.store(true, std::memory_order_release);
      queue.drain_and_reset(domesticate, [&](freelist::HeadPtr value) {
        SNMALLOC_CHECK(address_cast(value) == address_cast(as_head(object)));
        callbacks.fetch_add(1, std::memory_order_relaxed);
      });
      completed.store(true, std::memory_order_release);
    });

    wait_until([&]() { return entered.load(std::memory_order_acquire); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    SNMALLOC_CHECK(!completed.load(std::memory_order_acquire));
    SNMALLOC_CHECK(callbacks.load(std::memory_order_relaxed) == 0);

    queue.front.store(capptr_rewild(as_head(object)));
    consumer.join();

    SNMALLOC_CHECK(callbacks.load(std::memory_order_relaxed) == 1);
    SNMALLOC_CHECK(completed.load(std::memory_order_acquire));
    check_empty(queue);
  }

  void test_successor_publication_gap()
  {
    Queue queue;
    Object first;
    Object second;
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
    std::atomic<size_t> callbacks{0};

    SNMALLOC_CHECK(queue.enqueue(as_head(first), as_head(first), domesticate));

    freelist::Object::atomic_store_null(
      as_head(second), queue_key, NO_KEY_TWEAK);
    auto prev = queue.back.exchange(
      capptr_rewild(as_head(second)), stl::memory_order_acq_rel);
    SNMALLOC_CHECK(address_cast(prev) == address_cast(as_head(first)));

    std::thread consumer([&]() {
      entered.store(true, std::memory_order_release);
      queue.drain_and_reset(domesticate, [&callbacks](freelist::HeadPtr) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
      });
      completed.store(true, std::memory_order_release);
    });

    wait_until([&]() { return entered.load(std::memory_order_acquire); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    SNMALLOC_CHECK(!completed.load(std::memory_order_acquire));
    SNMALLOC_CHECK(callbacks.load(std::memory_order_relaxed) == 0);

    freelist::Object::atomic_store_next(
      as_head(first), as_head(second), queue_key, NO_KEY_TWEAK);
    consumer.join();

    SNMALLOC_CHECK(callbacks.load(std::memory_order_relaxed) == 2);
    SNMALLOC_CHECK(completed.load(std::memory_order_acquire));
    check_empty(queue);
  }

  void test_rejected_front()
  {
    Queue queue;
    Object object;
    std::atomic<bool> reject{true};
    std::atomic<bool> attempted{false};
    std::atomic<bool> completed{false};
    std::atomic<size_t> callbacks{0};

    SNMALLOC_CHECK(
      queue.enqueue(as_head(object), as_head(object), domesticate));

    auto validating_domesticate = [&reject,
                                   &attempted](freelist::QueuePtr value) {
      attempted.store(true, std::memory_order_release);
      if (reject.load(std::memory_order_acquire))
        return freelist::HeadPtr(nullptr);
      return domesticate(value);
    };

    std::thread consumer([&]() {
      queue.drain_and_reset(
        validating_domesticate, [&callbacks](freelist::HeadPtr) {
          callbacks.fetch_add(1, std::memory_order_relaxed);
        });
      completed.store(true, std::memory_order_release);
    });

    wait_until([&]() { return attempted.load(std::memory_order_acquire); });
    SNMALLOC_CHECK(!completed.load(std::memory_order_acquire));
    SNMALLOC_CHECK(callbacks.load(std::memory_order_relaxed) == 0);

    reject.store(false, std::memory_order_release);
    consumer.join();

    SNMALLOC_CHECK(callbacks.load(std::memory_order_relaxed) == 1);
    SNMALLOC_CHECK(completed.load(std::memory_order_acquire));
    check_empty(queue);
  }

  void test_callback_starts_replacement()
  {
    Queue queue;
    Object objects[4];
    CallbackOrder order;
    std::atomic<bool> first_callback{false};
    std::atomic<bool> release_callback{false};

    freelist::Object::atomic_store_next(
      as_head(objects[0]), as_head(objects[1]), queue_key, NO_KEY_TWEAK);
    SNMALLOC_CHECK(
      queue.enqueue(as_head(objects[0]), as_head(objects[1]), domesticate));

    std::thread consumer([&]() {
      queue.drain_and_reset(domesticate, [&](freelist::HeadPtr value) {
        order.add(value);
        if (address_cast(value) == address_cast(as_head(objects[0])))
        {
          first_callback.store(true, std::memory_order_release);
          wait_until(
            [&]() { return release_callback.load(std::memory_order_acquire); });
        }
      });
    });

    wait_until(
      [&]() { return first_callback.load(std::memory_order_acquire); });

    freelist::Object::atomic_store_next(
      as_head(objects[2]), as_head(objects[3]), queue_key, NO_KEY_TWEAK);
    SNMALLOC_CHECK(
      queue.enqueue(as_head(objects[2]), as_head(objects[3]), domesticate));
    release_callback.store(true, std::memory_order_release);
    consumer.join();

    SNMALLOC_CHECK(order.count == 2);
    for (size_t i = 0; i < 2; i++)
    {
      SNMALLOC_CHECK(
        address_cast(order.values[i]) == address_cast(as_head(objects[i])));
    }
    SNMALLOC_CHECK(queue.back.load(stl::memory_order_relaxed) != nullptr);

    order.count = 0;
    queue.drain_and_reset(
      domesticate, [&order](freelist::HeadPtr value) { order.add(value); });
    SNMALLOC_CHECK(order.count == 2);
    for (size_t i = 0; i < 2; i++)
    {
      SNMALLOC_CHECK(
        address_cast(order.values[i]) == address_cast(as_head(objects[i + 2])));
    }
    check_empty(queue);
  }

  size_t find_object(Object* objects, size_t count, freelist::HeadPtr value)
  {
    for (size_t i = 0; i < count; i++)
    {
      if (address_cast(as_head(objects[i])) == address_cast(value))
        return i;
    }
    SNMALLOC_CHECK(false);
    return count;
  }

  void test_concurrent_producers(size_t object_count)
  {
    constexpr size_t producer_count = 4;
    Queue queue;
    auto objects = std::make_unique<Object[]>(object_count);
    auto seen = std::make_unique<std::atomic<size_t>[]>(object_count);
    std::atomic<size_t> next_object{0};
    std::atomic<size_t> producers_live{producer_count};
    std::atomic<size_t> true_results{0};

    for (size_t i = 0; i < object_count; i++)
      seen[i].store(0, std::memory_order_relaxed);

    std::vector<std::thread> producers;
    for (size_t producer = 0; producer < producer_count; producer++)
    {
      producers.emplace_back([&]() {
        while (true)
        {
          size_t first_index =
            next_object.fetch_add(4, std::memory_order_relaxed);
          if (first_index >= object_count)
            break;

          size_t last_index = first_index + 3;
          if (last_index >= object_count)
            last_index = object_count - 1;
          for (size_t i = first_index; i < last_index; i++)
          {
            freelist::Object::atomic_store_next(
              as_head(objects[i]),
              as_head(objects[i + 1]),
              queue_key,
              NO_KEY_TWEAK);
          }

          if (queue.enqueue(
                as_head(objects[first_index]),
                as_head(objects[last_index]),
                domesticate))
          {
            true_results.fetch_add(1, std::memory_order_relaxed);
          }
        }
        producers_live.fetch_sub(1, std::memory_order_release);
      });
    }

    size_t callbacks = 0;
    while (producers_live.load(std::memory_order_acquire) != 0 ||
           queue.back.load(stl::memory_order_relaxed) != nullptr)
    {
      queue.drain_and_reset(domesticate, [&](freelist::HeadPtr value) {
        size_t index = find_object(objects.get(), object_count, value);
        SNMALLOC_CHECK(
          seen[index].fetch_add(1, std::memory_order_relaxed) == 0);
        callbacks++;
      });
      std::this_thread::yield();
    }

    for (auto& producer : producers)
      producer.join();

    SNMALLOC_CHECK(callbacks == object_count);
    for (size_t i = 0; i < object_count; i++)
      SNMALLOC_CHECK(seen[i].load(std::memory_order_relaxed) == 1);
    SNMALLOC_CHECK(true_results.load(std::memory_order_relaxed) >= 1);
    SNMALLOC_CHECK(
      true_results.load(std::memory_order_relaxed) <= object_count);
    check_empty(queue);

    Object replacement;
    SNMALLOC_CHECK(
      queue.enqueue(as_head(replacement), as_head(replacement), domesticate));
    SNMALLOC_CHECK(queue.back.load(stl::memory_order_relaxed) != nullptr);
    queue.drain_and_reset(domesticate, [](freelist::HeadPtr) {});
    check_empty(queue);
  }

  void test_reuse(size_t generations)
  {
    constexpr size_t object_count = 8;
    constexpr size_t reserved = 2;
    Queue queue;
    Object objects[object_count];
    std::atomic<size_t> states[object_count];
    std::atomic<size_t> generation[object_count];
    auto seen =
      std::make_unique<std::atomic<size_t>[]>(object_count * generations);
    std::atomic<bool> producer_done{false};

    for (size_t i = 0; i < object_count; i++)
    {
      states[i].store(0, std::memory_order_relaxed);
      generation[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < object_count * generations; i++)
      seen[i].store(0, std::memory_order_relaxed);

    std::thread producer([&]() {
      for (size_t round = 0; round < generations; round++)
      {
        size_t claimed[object_count];
        size_t claim_count = 0;
        while (claim_count < object_count)
        {
          for (size_t i = 0; i < object_count; i++)
          {
            size_t available = 0;
            if (states[i].compare_exchange_strong(
                  available,
                  reserved,
                  std::memory_order_acq_rel,
                  std::memory_order_relaxed))
            {
              generation[i].store(round, std::memory_order_relaxed);
              claimed[claim_count++] = i;
            }
          }
          if (claim_count < object_count)
            std::this_thread::yield();
        }

        for (size_t first = 0; first < object_count; first += 3)
        {
          size_t last = first + 2;
          if (last >= object_count)
            last = object_count - 1;
          for (size_t i = first; i < last; i++)
          {
            freelist::Object::atomic_store_next(
              as_head(objects[claimed[i]]),
              as_head(objects[claimed[i + 1]]),
              queue_key,
              NO_KEY_TWEAK);
          }
          for (size_t i = first; i <= last; i++)
            states[claimed[i]].store(1, std::memory_order_release);
          queue.enqueue(
            as_head(objects[claimed[first]]),
            as_head(objects[claimed[last]]),
            domesticate);
        }
      }
      producer_done.store(true, std::memory_order_release);
    });

    size_t callbacks = 0;
    while (!producer_done.load(std::memory_order_acquire) ||
           queue.back.load(stl::memory_order_relaxed) != nullptr)
    {
      queue.drain_and_reset(domesticate, [&](freelist::HeadPtr value) {
        size_t index = find_object(objects, object_count, value);
        SNMALLOC_CHECK(states[index].load(std::memory_order_acquire) == 1);
        size_t round = generation[index].load(std::memory_order_relaxed);
        SNMALLOC_CHECK(
          seen[(round * object_count) + index].fetch_add(
            1, std::memory_order_relaxed) == 0);
        callbacks++;
        states[index].store(0, std::memory_order_release);
        std::this_thread::yield();
      });
    }
    producer.join();

    SNMALLOC_CHECK(callbacks == object_count * generations);
    for (size_t i = 0; i < object_count * generations; i++)
      SNMALLOC_CHECK(seen[i].load(std::memory_order_relaxed) == 1);
    check_empty(queue);
  }
}

int main(int argc, char** argv)
{
  bool heavy = false;
  for (int i = 1; i < argc; i++)
  {
    if (std::strcmp(argv[i], "--heavy") == 0)
      heavy = true;
  }

  setup();
  test_empty();
  test_enqueue_and_iterate();
  test_dequeue_empty();
  test_dequeue_positions();
  test_dequeue_publication_gap();
  test_dequeue_checks_bound_first();
  test_remote_allocator_move_only_callback();
  test_front_publication_gap();
  test_successor_publication_gap();
  test_rejected_front();
  test_callback_starts_replacement();
  test_concurrent_producers(heavy ? 4096 : 256);
  test_reuse(heavy ? 256 : 16);
}
