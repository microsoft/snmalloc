#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  template<class T, uintptr_t terminator = 0>
  class DLList
  {
  private:
    static_assert(
      std::is_same<decltype(((T*)0)->prev), T*>::value, "T->prev must be a T*");
    static_assert(
      std::is_same<decltype(((T*)0)->next), T*>::value, "T->next must be a T*");

    T* head = (T*)terminator;

  public:
    T* get_head()
    {
      return head;
    }

    T* pop()
    {
      T* item = head;

      if (item != (T*)terminator)
        remove(item);

      return item;
    }

    void insert(T* item)
    {
#ifndef NDEBUG
      debug_check_not_contains(item);
#endif

      item->next = head;
      item->prev = (T*)terminator;

      if (head != (T*)terminator)
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

      if (item->next != (T*)terminator)
        item->next->prev = item->prev;

      if (item->prev != (T*)terminator)
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
        assert(curr != (T*)terminator);
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

      while (curr != (T*)terminator)
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
      T* prev = (T*)terminator;

      while (item != (T*)terminator)
      {
        assert(item->prev == prev);
        prev = item;
        item = item->next;
      }
#endif
    }
  };
}
