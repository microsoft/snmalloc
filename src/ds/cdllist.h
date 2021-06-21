#pragma once

#include "address.h"
#include "defines.h"
#include "ptrwrap.h"

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
  template<template<typename> typename Ptr = Pointer>
  class CDLLNode
  {
    Ptr<CDLLNode> next{nullptr};
    Ptr<CDLLNode> prev{nullptr};

    constexpr void set_next(Ptr<CDLLNode> c)
    {
      next = c;
    }

  public:
    /**
     * Single element cyclic list.  This is the empty case.
     */
    constexpr CDLLNode()
    {
      this->set_next(Ptr<CDLLNode>(this));
      prev = Ptr<CDLLNode>(this);
    }

    SNMALLOC_FAST_PATH bool is_empty()
    {
      return next == this;
    }

    SNMALLOC_FAST_PATH Ptr<CDLLNode> get_next()
    {
      return next;
    }

    /**
     * Single element cyclic list.  This is the uninitialised case.
     *
     * This entry should never be accessed and is only used to make
     * a fake metaslab.
     */
    constexpr CDLLNode(bool) {}

    /**
     * Removes this element from the cyclic list is it part of.
     */
    SNMALLOC_FAST_PATH void remove()
    {
      SNMALLOC_ASSERT(!this->is_empty());
      debug_check();
      this->get_next()->prev = prev;
      prev->set_next(this->get_next());
      // As this is no longer in the list, check invariant for
      // neighbouring element.
      this->get_next()->debug_check();

#ifndef NDEBUG
      this->set_next(nullptr);
      prev = nullptr;
#endif
    }

    /**
     * Nulls the previous pointer
     *
     * The Meta-slab uses nullptr in prev to mean that it is not part of a
     * size class list.
     **/
    void null_prev()
    {
      prev = nullptr;
    }

    SNMALLOC_FAST_PATH Ptr<CDLLNode> get_prev()
    {
      return prev;
    }

    SNMALLOC_FAST_PATH void insert_next(Ptr<CDLLNode> item)
    {
      debug_check();
      item->set_next(this->get_next());
      this->get_next()->prev = item;
      item->prev = this;
      set_next(item);
      debug_check();
    }

    SNMALLOC_FAST_PATH void insert_prev(Ptr<CDLLNode> item)
    {
      debug_check();
      item->prev = prev;
      prev->set_next(item);
      item->set_next(Ptr<CDLLNode>(this));
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
      Ptr<CDLLNode> item = this->get_next();
      auto p = Ptr<CDLLNode>(this);

      do
      {
        SNMALLOC_ASSERT(item->prev == p);
        p = item;
        item = item->get_next();
      } while (item != Ptr<CDLLNode>(this));
#endif
    }
  };
} // namespace snmalloc
