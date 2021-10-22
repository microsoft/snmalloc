#pragma once

#include "address.h"
#include "defines.h"
#include "ptrwrap.h"

#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  /**
   * Simple sequential queue of T.
   *
   * Linked using the T::next field.
   */
  template<typename T>
  class SeqQueue
  {
    static_assert(
      std::is_same<decltype(T::next), T*>::value,
      "T->next must be a queue pointer to T");
    T* head{nullptr};
    T** end{&head};

  public:
    /**
     * Empty queue
     */
    constexpr SeqQueue() = default;

    /**
     * Check for empty
     */
    SNMALLOC_FAST_PATH bool is_empty()
    {
      SNMALLOC_ASSERT(end != nullptr);
      return &head == end;
    }

    /**
     * Remove an element from the queue
     *
     * Assumes queue is non-empty
     */
    SNMALLOC_FAST_PATH T* pop()
    {
      SNMALLOC_ASSERT(!this->is_empty());
      auto result = head;
      if (&(head->next) == end)
        end = &head;
      else
        head = head->next;
      return result;
    }

    /**
     * Filter
     *
     * Removes all elements that f returns true for.
     * If f returns true, then filter is not allowed to look at the
     * object again, and f is responsible for its lifetime.
     */
    template<typename Fn>
    SNMALLOC_FAST_PATH void filter(Fn&& f)
    {
      T** prev = &head;
      // Check for empty case.
      if (prev == end)
        return;

      while (true)
      {
        T* curr = *prev;
        // Note must read curr->next before calling `f` as `f` is allowed to
        // mutate that field.
        T* next = curr->next;
        if (f(curr))
        {
          // Remove element;
          *prev = next;
        }
        else
        {
          // Keep element
          prev = &(curr->next);
        }

        if (&(curr->next) == end)
          break;
      }

      end = prev;
    }

    /**
     * Add an element to the queue.
     */
    SNMALLOC_FAST_PATH void insert(T* item)
    {
      *end = item;
      end = &(item->next);
    }
  };
} // namespace snmalloc
