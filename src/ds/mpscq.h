#pragma once

#include "bits.h"
#include "helpers.h"

#include <utility>
namespace snmalloc
{
  template<class T>
  class MPSCQ
  {
  private:
    static_assert(
      std::is_same<decltype(T::next), std::atomic<T*>>::value,
      "T->next must be a std::atomic<T*>");

    std::atomic<T*> back = nullptr;
    T* front = nullptr;

  public:
    void invariant()
    {
      SNMALLOC_ASSERT(back != nullptr);
      SNMALLOC_ASSERT(front != nullptr);
    }

    void init(T* stub)
    {
      stub->next.store(nullptr, std::memory_order_relaxed);
      front = stub;
      back.store(stub, std::memory_order_relaxed);
      invariant();
    }

    T* destroy()
    {
      T* fnt = front;
      back.store(nullptr, std::memory_order_relaxed);
      front = nullptr;
      return fnt;
    }

    inline bool is_empty()
    {
      T* bk = back.load(std::memory_order_relaxed);

      return bk == front;
    }

    void enqueue(T* first, T* last)
    {
      // Pushes a list of messages to the queue. Each message from first to
      // last should be linked together through their next pointers.
      invariant();
      last->next.store(nullptr, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      T* prev = back.exchange(last, std::memory_order_relaxed);
      prev->next.store(first, std::memory_order_relaxed);
    }

    std::pair<T*, bool> dequeue()
    {
      // Returns the front message, or null if not possible to return a message.
      invariant();
      T* first = front;
      T* next = first->next.load(std::memory_order_relaxed);

      if (next != nullptr)
      {
        front = next;
        Aal::prefetch(&(next->next));
        SNMALLOC_ASSERT(front);
        std::atomic_thread_fence(std::memory_order_acquire);
        invariant();
        return std::pair(first, true);
      }

      return std::pair(nullptr, false);
    }
  };
} // namespace snmalloc
