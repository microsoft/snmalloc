#pragma once

#include "aba.h"

namespace snmalloc
{
  template<
    class T,
    Construction c = RequiresInit,
    template<typename> typename Ptr = Pointer,
    template<typename> typename AtomicPtr = AtomicPointer>
  class MPMCStack
  {
    using ABAT = ABA<T, c, Ptr, AtomicPtr>;

  private:
    static_assert(
      std::is_same<decltype(T::next), AtomicPtr<T>>::value,
      "T->next must be an AtomicPtr<T>");

    ABAT stack;

  public:
    void push(Ptr<T> item)
    {
      return push(item, item);
    }

    void push(Ptr<T> first, Ptr<T> last)
    {
      // Pushes an item on the stack.
      auto cmp = stack.read();

      do
      {
        Ptr<T> top = cmp.ptr();
        last->next.store(top, std::memory_order_release);
      } while (!cmp.store_conditional(first));
    }

    Ptr<T> pop()
    {
      // Returns the next item. If the returned value is decommitted, it is
      // possible for the read of top->next to segfault.
      auto cmp = stack.read();
      Ptr<T> top;
      Ptr<T> next;

      do
      {
        top = cmp.ptr();

        if (top == nullptr)
          break;

        next = top->next.load(std::memory_order_acquire);
      } while (!cmp.store_conditional(next));

      return top;
    }

    Ptr<T> pop_all()
    {
      // Returns all items as a linked list, leaving an empty stack.
      auto cmp = stack.read();
      Ptr<T> top;

      do
      {
        top = cmp.ptr();

        if (top == nullptr)
          break;
      } while (!cmp.store_conditional(nullptr));

      return top;
    }
  };
} // namespace snmalloc
