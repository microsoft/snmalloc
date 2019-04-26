#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  template<class T, uintptr_t Terminator = 0>
  class DLList
  {
  private:
    // Single point to perform this cast.
    // Would like this to be a constexpr, but reinterpret cast is not allowed in
    // constexpr.
    static inline T* terminator()
    {
      return (T*)Terminator;
    }

    static_assert(
      std::is_same<decltype(T::prev), T*>::value, "T->prev must be a T*");
    static_assert(
      std::is_same<decltype(T::next), T*>::value, "T->next must be a T*");

    T* head = terminator();

  public:
    bool is_empty()
    {
      return head == terminator();
    }

    T* get_head()
    {
      return head;
    }

    T* pop()
    {
      T* item = head;

      if (item != terminator())
        remove(item);

      return item;
    }

    void insert(T* item)
    {
#ifndef NDEBUG
      debug_check_not_contains(item);
#endif

      item->next = head;
      item->prev = terminator();

      if (head != terminator())
        head->prev = item;

      head = item;
#ifndef NDEBUG
      debug_check();
#endif
    }

    void remove(T* item)
    {
#ifndef NDEBUG
      debug_check_contains(item);
#endif

      if (item->next != terminator())
        item->next->prev = item->prev;

      if (item->prev != terminator())
        item->prev->next = item->next;
      else
        head = item->next;

#ifndef NDEBUG
      debug_check();
#endif
    }

    void debug_check_contains(T* item)
    {
#ifndef NDEBUG
      debug_check();
      T* curr = head;

      while (curr != item)
      {
        assert(curr != terminator());
        curr = curr->next;
      }
#else
      UNUSED(item);
#endif
    }

    void debug_check_not_contains(T* item)
    {
#ifndef NDEBUG
      debug_check();
      T* curr = head;

      while (curr != terminator())
      {
        assert(curr != item);
        curr = curr->next;
      }
#else
      UNUSED(item);
#endif
    }

    void debug_check()
    {
#ifndef NDEBUG
      T* item = head;
      T* prev = terminator();

      while (item != terminator())
      {
        assert(item->prev == prev);
        prev = item;
        item = item->next;
      }
#endif
    }
  };
}
