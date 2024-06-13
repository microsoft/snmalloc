#pragma once

#include "../ds_core/ds_core.h"
#include "aba.h"
#include "allocconfig.h"

namespace snmalloc
{
  template<class T, Construction c = RequiresInit>
  class MPMCStack
  {
    using ABAT = ABA<T, c>;

  private:
    alignas(CACHELINE_SIZE) ABAT stack;

#ifdef SNMALLOC_THREAD_SANITIZER_ENABLED
    __attribute__((no_sanitize("thread"))) static T*
    racy_read(std::atomic<T*>& ptr)
    {
      // reinterpret_cast is required as TSAN still instruments
      // std::atomic operations, even if you disable TSAN on
      // the function.
      return *reinterpret_cast<T**>(&ptr);
    }
#else
    static T* racy_read(std::atomic<T*>& ptr)
    {
      return ptr.load(std::memory_order_relaxed);
    }
#endif

  public:
    constexpr MPMCStack() = default;

    void push(T* item)
    {
      static_assert(
        std::is_same<decltype(T::next), std::atomic<T*>>::value,
        "T->next must be an std::atomic<T*>");

      return push(item, item);
    }

    void push(T* first, T* last)
    {
      // Pushes an item on the stack.
      auto cmp = stack.read();

      do
      {
        auto top = cmp.ptr();
        last->next.store(top, std::memory_order_release);
      } while (!cmp.store_conditional(first));
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
        top = cmp.ptr();

        if (top == nullptr)
          break;

        // The following read can race with non-atomic accesses
        // this is undefined behaviour. There is no way to use
        // CAS sensibly that conforms to the standard with optimistic
        // concurrency.
        next = racy_read(top->next);
      } while (!cmp.store_conditional(next));

      return top;
    }

    T* pop_all()
    {
      // Returns all items as a linked list, leaving an empty stack.
      auto cmp = stack.read();
      T* top;

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
