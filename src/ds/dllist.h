#pragma once

#include "address.h"
#include "helpers.h"
#include "invalidptr.h"
#include "ptrwrap.h"

#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  template<
    class T,
    template<typename> typename Ptr = Pointer,
    class Terminator = std::nullptr_t,
    void on_clear(Ptr<T>) = ignore<T, Ptr>>
  class DLList final
  {
  private:
    static_assert(
      std::is_same<decltype(T::prev), Ptr<T>>::value,
      "T->prev must be a Ptr<T>");
    static_assert(
      std::is_same<decltype(T::next), Ptr<T>>::value,
      "T->next must be a Ptr<T>");

    Ptr<T> head = Terminator();
    Ptr<T> tail = Terminator();

  public:
    ~DLList()
    {
      clear();
    }

    DLList() = default;

    DLList(DLList&& o) noexcept
    {
      head = o.head;
      tail = o.tail;

      o.head = nullptr;
      o.tail = nullptr;
    }

    DLList& operator=(DLList&& o) noexcept
    {
      head = o.head;
      tail = o.tail;

      o.head = nullptr;
      o.tail = nullptr;
      return *this;
    }

    SNMALLOC_FAST_PATH bool is_empty()
    {
      return head == Terminator();
    }

    SNMALLOC_FAST_PATH Ptr<T> get_head()
    {
      return head;
    }

    Ptr<T> get_tail()
    {
      return tail;
    }

    SNMALLOC_FAST_PATH Ptr<T> pop()
    {
      Ptr<T> item = head;

      if (item != Terminator())
        remove(item);

      return item;
    }

    Ptr<T> pop_tail()
    {
      Ptr<T> item = tail;

      if (item != Terminator())
        remove(item);

      return item;
    }

    void insert(Ptr<T> item)
    {
#ifndef NDEBUG
      debug_check_not_contains(item);
#endif

      item->next = head;
      item->prev = Terminator();

      if (head != Terminator())
        head->prev = item;
      else
        tail = item;

      head = item;
#ifndef NDEBUG
      debug_check();
#endif
    }

    void insert_back(Ptr<T> item)
    {
#ifndef NDEBUG
      debug_check_not_contains(item);
#endif

      item->prev = tail;
      item->next = Terminator();

      if (tail != Terminator())
        tail->next = item;
      else
        head = item;

      tail = item;
#ifndef NDEBUG
      debug_check();
#endif
    }

    SNMALLOC_FAST_PATH void remove(Ptr<T> item)
    {
#ifndef NDEBUG
      debug_check_contains(item);
#endif

      if (item->next != Terminator())
        item->next->prev = item->prev;
      else
        tail = item->prev;

      if (item->prev != Terminator())
        item->prev->next = item->next;
      else
        head = item->next;

#ifndef NDEBUG
      debug_check();
#endif
    }

    void clear()
    {
      while (head != nullptr)
      {
        auto c = head;
        remove(c);
        on_clear(c);
      }
    }

    void debug_check_contains(Ptr<T> item)
    {
#ifndef NDEBUG
      debug_check();
      Ptr<T> curr = head;

      while (curr != item)
      {
        SNMALLOC_ASSERT(curr != Terminator());
        curr = curr->next;
      }
#else
      UNUSED(item);
#endif
    }

    void debug_check_not_contains(Ptr<T> item)
    {
#ifndef NDEBUG
      debug_check();
      Ptr<T> curr = head;

      while (curr != Terminator())
      {
        SNMALLOC_ASSERT(curr != item);
        curr = curr->next;
      }
#else
      UNUSED(item);
#endif
    }

    void debug_check()
    {
#ifndef NDEBUG
      Ptr<T> item = head;
      Ptr<T> prev = Terminator();

      while (item != Terminator())
      {
        SNMALLOC_ASSERT(item->prev == prev);
        prev = item;
        item = item->next;
      }
#endif
    }
  };
} // namespace snmalloc
