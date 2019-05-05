#pragma once

#include "aba.h"

namespace snmalloc
{
  template<class T, Construction c = RequiresInit>
  class MPMCStack
  {
    using ABAT = ABA<T, c>;

  private:
    static_assert(
      std::is_same<decltype(T::next), std::atomic<T*>>::value,
      "T->next must be a std::atomic<T*>");

    ABAT stack;

  public:
    void push(T* item)
    {
      return push(item, item);
    }

    void push(T* first, T* last)
    {
      // Pushes an item on the stack.
      auto cmp = stack.read();

      do
      {
        T* top = ABAT::load(cmp);
        last->next.store(top, std::memory_order_release);
      } while (!stack.compare_exchange(cmp, first));
    }

    T* pop()
    {
      // Returns the next item. If the returned value is decommitted, it is
      // possible for the read of top->next to segfault.
      auto cmp = stack.read();
      T* top;
      T* next;

      do
      {
        top = ABAT::load(cmp);

        if (top == nullptr)
          break;

        next = top->next.load(std::memory_order_acquire);
      } while (!stack.compare_exchange(cmp, next));

      return top;
    }

    T* pop_all()
    {
      // Returns all items as a linked list, leaving an empty stack.
      auto cmp = stack.read();
      T* top;

      do
      {
        top = ABAT::load(cmp);

        if (top == nullptr)
          break;
      } while (!stack.compare_exchange(cmp, nullptr));

      return top;
    }
  };
} // namespace snmalloc
