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
     * This sequence structure is intrusive, in that it requires the use of a
     * `next` field in the elements it manages, but, unlike some other intrusive
     * designs, it does not require the use of a `container_of`-like construct,
     * because its pointers point to the element, not merely the intrusive
     * member.
     *
     * In some cases, the next pointer is provided by a superclass but the list
     * is templated over the subclass.  The `SeqSet` enforces the invariant that
     * only instances of the subclass can be added to the list and so can safely
     * down-cast the type of `.next` to `T*`.  As such, we require only that the
     * `next` field is a pointer to `T` or some superclass of `T`.
     * %{
     */
    using NextPtr = decltype(std::declval<T>().next);
    static_assert(
      std::is_base_of_v<std::remove_pointer_t<NextPtr>, T>,
      "T->next must be a queue pointer to T");
    ///@}

    /**
     * Field representation for Fifo behaviour.
     */
    struct FieldFifo
    {
      NextPtr head{nullptr};
    };

    /**
     * Field representation for Lifo behaviour.
     */
    struct FieldLifo
    {
      NextPtr head{nullptr};
      NextPtr* end{&head};
    };

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
      // This cast is safe if the ->next pointers in all of the objects in the
      // list are managed by this class because object types are checked on
      // insertion.
      return static_cast<T*>(result);
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

      NextPtr* prev = &(v.head);

      while (true)
      {
        if constexpr (Fifo)
        {
          if (*prev == nullptr)
            break;
        }

        NextPtr curr = *prev;
        // Note must read curr->next before calling `f` as `f` is allowed to
        // mutate that field.
        NextPtr next = curr->next;
        if (f(static_cast<T*>(curr)))
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

    /**
     * Peek at next element in the set.
     */
    SNMALLOC_FAST_PATH const T* peek()
    {
      return static_cast<T*>(v.head);
    }
  };
} // namespace snmalloc
