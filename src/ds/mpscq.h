#pragma once

#include "bits.h"
#include "helpers.h"

#include <utility>
namespace snmalloc
{
  template<
    class T,
    template<typename> typename Ptr = Pointer,
    template<typename> typename AtomicPtr = AtomicPointer>
  class MPSCQ
  {
  private:
    static_assert(
      std::is_same<decltype(T::next), AtomicPtr<T>>::value,
      "T->next must be an AtomicPtr<T>");

    AtomicPtr<T> back{nullptr};
    Ptr<T> front{nullptr};

  public:
    constexpr MPSCQ() {};

    void invariant()
    {
      SNMALLOC_ASSERT(back != nullptr);
      SNMALLOC_ASSERT(front != nullptr);
    }

    void init(Ptr<T> stub)
    {
      stub->next.store(nullptr, std::memory_order_relaxed);
      front = stub;
      back.store(stub, std::memory_order_relaxed);
      invariant();
    }

    Ptr<T> destroy()
    {
      Ptr<T> fnt = front;
      back.store(nullptr, std::memory_order_relaxed);
      front = nullptr;
      return fnt;
    }

    inline bool is_empty()
    {
      Ptr<T> bk = back.load(std::memory_order_relaxed);

      return bk == front;
    }

    void enqueue(Ptr<T> first, Ptr<T> last)
    {
      // Pushes a list of messages to the queue. Each message from first to
      // last should be linked together through their next pointers.
      invariant();
      last->next.store(nullptr, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      Ptr<T> prev = back.exchange(last, std::memory_order_relaxed);
      prev->next.store(first, std::memory_order_relaxed);
    }

    std::pair<Ptr<T>, bool> dequeue()
    {
      // Returns the front message, or null if not possible to return a message.
      invariant();
      Ptr<T> first = front;
      Ptr<T> next = first->next.load(std::memory_order_relaxed);

      if (next != nullptr)
      {
        front = next;
        Aal::prefetch(&(next->next));
        SNMALLOC_ASSERT(front != nullptr);
        std::atomic_thread_fence(std::memory_order_acquire);
        invariant();
        return {first, true};
      }

      return {nullptr, false};
    }
  };
} // namespace snmalloc
