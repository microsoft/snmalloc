#pragma once

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  /**
   * Invalid pointer class.  This is similar to `std::nullptr_t`, but allows
   * other values.
   */
  template<address_t Sentinel>
  struct InvalidPointer
  {
    /**
     * Equality comparison. Two invalid pointer values with the same sentinel
     * are always the same, invalid pointer values with different sentinels are
     * always different.
     */
    template<uintptr_t OtherSentinel>
    constexpr bool operator==(const InvalidPointer<OtherSentinel>&)
    {
      return Sentinel == OtherSentinel;
    }
    /**
     * Equality comparison. Two invalid pointer values with the same sentinel
     * are always the same, invalid pointer values with different sentinels are
     * always different.
     */
    template<uintptr_t OtherSentinel>
    constexpr bool operator!=(const InvalidPointer<OtherSentinel>&)
    {
      return Sentinel != OtherSentinel;
    }
    /**
     * Implicit conversion, creates a pointer with the value of the sentinel.
     * On CHERI and other provenance-tracking systems, this is a
     * provenance-free integer and so will trap if dereferenced, on other
     * systems the sentinel should be a value in unmapped memory.
     */
    template<typename T>
    operator T*() const
    {
      return reinterpret_cast<T*>(Sentinel);
    }
    /**
     * Implicit conversion to an address, returns the sentinel value.
     */
    operator address_t() const
    {
      return Sentinel;
    }
  };

  template<class T, class Terminator = std::nullptr_t>
  class DLList
  {
  private:
    static_assert(
      std::is_same<decltype(T::prev), T*>::value, "T->prev must be a T*");
    static_assert(
      std::is_same<decltype(T::next), T*>::value, "T->next must be a T*");

    T* head = Terminator();

  public:
    bool is_empty()
    {
      return head == Terminator();
    }

    T* get_head()
    {
      return head;
    }

    T* pop()
    {
      T* item = head;

      if (item != Terminator())
        remove(item);

      return item;
    }

    void insert(T* item)
    {
#ifndef NDEBUG
      debug_check_not_contains(item);
#endif

      item->next = head;
      item->prev = Terminator();

      if (head != Terminator())
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

      if (item->next != Terminator())
        item->next->prev = item->prev;

      if (item->prev != Terminator())
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
        assert(curr != Terminator());
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

      while (curr != Terminator())
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
      T* prev = Terminator();

      while (item != Terminator())
      {
        assert(item->prev == prev);
        prev = item;
        item = item->next;
      }
#endif
    }
  };
} // namespace snmalloc
