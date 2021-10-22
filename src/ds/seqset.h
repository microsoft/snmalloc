#pragma once

#include "address.h"
#include "defines.h"
#include "ptrwrap.h"

#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  /**
   * Simple sequential set of T.
   *
   * Linked using the T::next field.
   *
   * Can be used in either Fifo or Lifo mode, which is
   * specified by template parameter.
   */
  template<typename T, bool Fifo = false>
  class SeqSet
  {
    /**
     * Field representation for Fifo behaviour.
     */
    struct FieldFifo
    {
      T* head{nullptr};
    };

    /**
     * Field representation for Lifo behaviour.
     */
    struct FieldLifo
    {
      T* head{nullptr};
      T** end{&head};
    };

    static_assert(
      std::is_same<decltype(T::next), T*>::value,
      "T->next must be a queue pointer to T");

    /**
     * Field indirection to actual representation.
     * Different numbers of fields are required for the
     * two behaviours.
     */
    std::conditional_t<Fifo, FieldFifo, FieldLifo> v;

    /**
     * Check for empty
     */
    SNMALLOC_FAST_PATH bool is_empty()
    {
      if constexpr (Fifo)
      {
        return v.head == nullptr;
      }
      else
      {
        SNMALLOC_ASSERT(v.end != nullptr);
        return &(v.head) == v.end;
      }
    }

  public:
    /**
     * Empty queue
     */
    constexpr SeqSet() = default;

    /**
     * Remove an element from the queue
     *
     * Assumes queue is non-empty
     */
    SNMALLOC_FAST_PATH T* pop()
    {
      SNMALLOC_ASSERT(!this->is_empty());
      auto result = v.head;
      if constexpr (Fifo)
      {
        v.head = result->next;
      }
      else
      {
        if (&(v.head->next) == v.end)
          v.end = &(v.head);
        else
          v.head = v.head->next;
      }
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
      // Check for empty case.
      if (is_empty())
        return;

      T** prev = &(v.head);

      while (true)
      {
        if constexpr (Fifo)
        {
          if (*prev == nullptr)
            break;
        }

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
        if constexpr (!Fifo)
        {
          if (&(curr->next) == v.end)
            break;
        }
      }
      if constexpr (!Fifo)
      {
        v.end = prev;
      }
    }

    /**
     * Add an element to the queue.
     */
    SNMALLOC_FAST_PATH void insert(T* item)
    {
      if constexpr (Fifo)
      {
        item->next = v.head;
        v.head = item;
      }
      else
      {
        *(v.end) = item;
        v.end = &(item->next);
      }
    }
  };
} // namespace snmalloc
