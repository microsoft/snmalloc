#pragma once

#include "address.h"
#include "defines.h"
#include "ptrwrap.h"

#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  /**
   * TODO Rewrite for actual use, no longer Cyclic or doubly linked.
   *
   * Special class for cyclic doubly linked non-empty linked list
   *
   * This code assumes there is always one element in the list. The client
   * must ensure there is a sentinal element.
   */
  template<template<typename> typename Ptr = Pointer>
  class CDLLNode
  {
    Ptr<CDLLNode> next{nullptr};

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
      this->set_next(nullptr);
    }

    SNMALLOC_FAST_PATH bool is_empty()
    {
      return next == nullptr;
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

    SNMALLOC_FAST_PATH Ptr<CDLLNode> pop()
    {
      SNMALLOC_ASSERT(!this->is_empty());
      auto result = get_next();
      set_next(result->get_next());
      return result;
    }

    SNMALLOC_FAST_PATH void insert(Ptr<CDLLNode> item)
    {
      debug_check();
      item->set_next(this->get_next());
      set_next(item);
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
      // Ptr<CDLLNode> item = this->get_next();
      // auto p = Ptr<CDLLNode>(this);

      // do
      // {
      //   SNMALLOC_ASSERT(item->prev == p);
      //   p = item;
      //   item = item->get_next();
      // } while (item != Ptr<CDLLNode>(this));
#endif
    }
  };
} // namespace snmalloc
