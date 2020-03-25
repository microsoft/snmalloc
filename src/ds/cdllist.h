#pragma once

#include "defines.h"

#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  /**
   * Special class for cyclic doubly linked non-empty linked list
   *
   * This code assumes there is always one element in the list. The client
   * must ensure there is a sentinal element.
   */
  class CDLLNode
  {
    /**
     * to_next is used to handle a zero initialised data structure.
     * This means that `is_empty` works even when the constructor hasn't
     * been run.
     */
    ptrdiff_t to_next = 0;

    // TODO: CHERI will need a real pointer too
    //    CDLLNode* next = nullptr;
    CDLLNode* prev = nullptr;

    void set_next(CDLLNode* c)
    {
      // TODO: CHERI will need a real pointer too
      //      next = c;
      to_next = pointer_diff_signed(this, c);
    }

  public:
    /**
     * Single element cyclic list.  This is the empty case.
     */
    CDLLNode()
    {
      set_next(this);
      prev = this;
    }

    SNMALLOC_FAST_PATH bool is_empty()
    {
      return to_next == 0;
    }

    /**
     * Removes this element from the cyclic list is it part of.
     */
    SNMALLOC_FAST_PATH void remove()
    {
      SNMALLOC_ASSERT(!is_empty());
      debug_check();
      get_next()->prev = prev;
      prev->set_next(get_next());
      // As this is no longer in the list, check invariant for
      // neighbouring element.
      get_next()->debug_check();

#ifndef NDEBUG
      set_next(nullptr);
      prev = nullptr;
#endif
    }

    SNMALLOC_FAST_PATH CDLLNode* get_next()
    {
      // TODO: CHERI will require a real pointer
      //    return next;
      return pointer_offset_signed(this, to_next);
    }

    SNMALLOC_FAST_PATH CDLLNode* get_prev()
    {
      return prev;
    }

    SNMALLOC_FAST_PATH void insert_next(CDLLNode* item)
    {
      debug_check();
      item->set_next(get_next());
      get_next()->prev = item;
      item->prev = this;
      set_next(item);
      debug_check();
    }

    SNMALLOC_FAST_PATH void insert_prev(CDLLNode* item)
    {
      debug_check();
      item->prev = prev;
      prev->set_next(item);
      item->set_next(this);
      prev = item;
      debug_check();
    }

    /**
     * Checks the lists invariants
     *   x->next->prev = x
     * for all x in the list.
     */
    void debug_check()
    {
#ifndef NDEBUG
      CDLLNode* item = get_next();
      CDLLNode* p = this;

      do
      {
        SNMALLOC_ASSERT(item->prev == p);
        p = item;
        item = item->get_next();
      } while (item != this);
#endif
    }
  };
} // namespace snmalloc
