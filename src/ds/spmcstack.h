#pragma once

#include "aba.h"
#include "ptrwrap.h"

namespace snmalloc
{
  /**
   * Concurrent Stack
   *
   * This stack supports the following clients
   * (push|pop)* || pop_all* || ... || pop_all*
   *
   * That is a single thread that can do push and pop, and other threads
   * that do pop_all.  pop_all if it returns a value, returns all of the
   * stack, however, it may return nullptr if it races with either a push
   * or a pop.
   *
   * The primary use case is single-threaded access, where other threads
   * can attempt to steal all the values.
   */
  template<class T>
  class SPMCStack
  {
  private:
    alignas(CACHELINE_SIZE) std::atomic<T*> stack{};

  public:
    constexpr SPMCStack() = default;

    void push(T* item)
    {
      static_assert(
        std::is_same<decltype(T::next), std::atomic<T*>>::value,
        "T->next must be an std::atomic<T*>");

      return push(item, item);
    }

    void push(T* first, T* last)
    {
      T* old_head = stack.exchange(nullptr, std::memory_order_relaxed);
      last->next.store(old_head, std::memory_order_relaxed);
      // Assume stays null as not allowed to race with pop or other pushes.
      SNMALLOC_ASSERT(stack.load() == nullptr);
      stack.store(first, std::memory_order_release);
    }

    T* pop()
    {
      if (stack.load(std::memory_order_relaxed) == nullptr)
        return nullptr;
      T* old_head = stack.exchange(nullptr);
      if (SNMALLOC_UNLIKELY(old_head == nullptr))
        return nullptr;

      auto next = old_head->next.load(std::memory_order_relaxed);

      // Assume stays null as not allowed to race with pop or other pushes.
      SNMALLOC_ASSERT(stack.load() == nullptr);

      stack.store(next, std::memory_order_release);

      return old_head;
    }

    T* pop_all()
    {
      return stack.exchange(nullptr);
    }
  };
} // namespace snmalloc
