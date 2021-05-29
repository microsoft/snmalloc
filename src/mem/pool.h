#pragma once

#include "../backend/backend.h"
#include "../ds/flaglock.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal_concept.h"
#include "pooled.h"

namespace snmalloc
{
  /**
   * Pool of a particular type of object.
   *
   * This pool will never return objects to the OS.  It maintains a list of all
   * objects ever allocated that can be iterated (not concurrency safe).  Pooled
   * types can be acquired from the pool, and released back to the pool. This is
   * concurrency safe.
   *
   * This is used to bootstrap the allocation of allocators.
   */
  template<class T>
  class PoolState
  {
    template<typename TT>
    friend class Pool;

  private:
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    MPMCStack<T, PreZeroed> stack;
    T* list = nullptr;

  public:
    constexpr PoolState() {}
  };

  template<typename T>
  class Pool
  {
  public:
    template<typename SharedStateHandle, typename... Args>
    static T* acquire(SharedStateHandle h, Args&&... args)
    {
      PoolState<T>& pool = h.pool();
      T* p = pool.stack.pop();

      if (p != nullptr)
      {
        p->set_in_use();
        return p;
      }

      p = BackendAllocator::alloc_meta_data<T>(h, std::forward<Args>(args)...);

      FlagLock f(pool.lock);
      p->list_next = pool.list;
      pool.list = p;

      p->set_in_use();
      return p;
    }

    /**
     * Return to the pool an object previously retrieved by `acquire`
     *
     * Do not return objects from `extract`.
     */
    template<typename SharedStateHandle>
    static void release(SharedStateHandle h, T* p)
    {
      // The object's destructor is not run. If the object is "reallocated", it
      // is returned without the constructor being run, so the object is reused
      // without re-initialisation.
      p->reset_in_use();
      h.pool().stack.push(p);
    }

    template<typename SharedStateHandle>
    static T* extract(SharedStateHandle h, T* p = nullptr)
    {
      // Returns a linked list of all objects in the stack, emptying the stack.
      if (p == nullptr)
        return h.pool().stack.pop_all();

      return p->next;
    }

    /**
     * Return to the pool a list of object previously retrieved by `extract`
     *
     * Do not return objects from `acquire`.
     */
    template<typename SharedStateHandle>
    static void restore(SharedStateHandle h, T* first, T* last)
    {
      // Pushes a linked list of objects onto the stack. Use to put a linked
      // list returned by extract back onto the stack.
      h.pool().stack.push(first, last);
    }

    template<typename SharedStateHandle>
    static T* iterate(SharedStateHandle h, T* p = nullptr)
    {
      if (p == nullptr)
        return h.pool().list;

      return p->list_next;
    }
  };
} // namespace snmalloc
