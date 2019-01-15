#pragma once

#include "bits.h"

#include <stdlib.h>
#include <utility>

namespace snmalloc
{
  template<class T>
  class MPSCQ
  {
  private:
    static_assert(
      std::is_same<decltype(((T*)0)->next), std::atomic<T*>>::value,
      "T->next must be a std::atomic<T*>");

    std::atomic<T*> head;
    T* tail;

  public:
    void invariant()
    {
#ifndef NDEBUG
      assert(head != nullptr);
      assert(tail != nullptr);
#endif
    }

    void init(T* stub)
    {
      stub->next.store(nullptr, std::memory_order_relaxed);
      tail = stub;
      head.store(stub, std::memory_order_relaxed);
      invariant();
    }

    T* destroy()
    {
      T* tl = tail;
      head.store(nullptr, std::memory_order_relaxed);
      tail = nullptr;
      return tl;
    }

    T* get_head()
    {
      return head.load(std::memory_order_relaxed);
    }

    inline void push(T* item)
    {
      push(item, item);
    }

    inline bool is_empty()
    {
      T* hd = head.load(std::memory_order_relaxed);

      return hd == tail;
    }

    void push(T* first, T* last)
    {
      // Pushes a list of messages to the queue. Each message from first to
      // last should be linked together through their next pointers.
      invariant();
      last->next.store(nullptr, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_release);
      T* prev = head.exchange(last, std::memory_order_relaxed);
      prev->next.store(first, std::memory_order_relaxed);
    }

    std::pair<T*, T*> pop()
    {
      // Returns the next message and the tail message. If the next message
      // is not null, the tail message should be freed by the caller.
      invariant();
      T* tl = tail;
      T* next = tl->next.load(std::memory_order_relaxed);

      if (next != nullptr)
      {
        tail = next;

        assert(tail);
        std::atomic_thread_fence(std::memory_order_acquire);
      }

      invariant();
      return std::make_pair(next, tl);
    }

    T* peek()
    {
      return tail->next.load(std::memory_order_relaxed);
    }
  };
}
