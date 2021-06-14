#pragma once

#include "address.h"
#include "defines.h"
#include "ptrwrap.h"

#include <cstdint>
#include <type_traits>

namespace snmalloc
{
  template<typename T, template<typename> typename Ptr = Pointer>
  class CDLLNodeBase
  {
    /**
     * to_next is used to handle a zero initialised data structure.
     * This means that `is_empty` works even when the constructor hasn't
     * been run.
     */
    ptrdiff_t to_next{0};

  protected:
    constexpr void set_next(Ptr<T> c)
    {
      to_next = pointer_diff_signed(Ptr<CDLLNodeBase<T, Ptr>>(this), c);
    }

  public:
    SNMALLOC_FAST_PATH bool is_empty()
    {
      return to_next == 0;
    }

    SNMALLOC_FAST_PATH Ptr<T> get_next()
    {
      return static_cast<Ptr<T>>(pointer_offset_signed<T>(this, to_next));
    }
  };

  template<typename T, template<typename> typename Ptr = Pointer>
  class CDLLNodeBaseNext
  {
    /**
     * Like to_next in the pointer-less case, this version still works with
     * zero-initialized data structure.  To make `is_empty` work in this case,
     * next is set to `nullptr` rather than `this` when the list is empty.
     *
     */

    Ptr<T> next{nullptr};

  protected:
    constexpr void set_next(Ptr<T> c)
    {
      next = address_cast(c) == address_cast(this) ? nullptr : c;
    }

  public:
    SNMALLOC_FAST_PATH bool is_empty()
    {
      return next == nullptr;
    }

    SNMALLOC_FAST_PATH Ptr<T> get_next()
    {
      return next == nullptr ? Ptr<T>(static_cast<T*>(this)) : next;
    }
  };

  template<typename T, template<typename> typename Ptr = Pointer>
  using CDLLNodeParent = std::conditional_t<
    aal_supports<StrictProvenance>,
    CDLLNodeBaseNext<T, Ptr>,
    CDLLNodeBase<T, Ptr>>;

  /**
   * Special class for cyclic doubly linked non-empty linked list
   *
   * This code assumes there is always one element in the list. The client
   * must ensure there is a sentinal element.
   */
  template<template<typename> typename Ptr = Pointer>
  class CDLLNode : public CDLLNodeParent<CDLLNode<Ptr>, Ptr>
  {
    Ptr<CDLLNode> prev{nullptr};

  public:
    /**
     * Single element cyclic list.  This is the empty case.
     */
    CDLLNode()
    {
      this->set_next(Ptr<CDLLNode>(this));
      prev = Ptr<CDLLNode>(this);
    }

    /**
     * Single element cyclic list.  This is the uninitialised case.
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
