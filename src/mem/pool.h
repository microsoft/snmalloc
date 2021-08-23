#pragma once

#include "../ds/flaglock.h"
#include "../ds/mpmcstack.h"
#include "../pal/pal_concept.h"
#include "pooled.h"
#include "slaballocator.h"

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
    template<typename TT, typename SharedStateHandle>
    friend class Pool;
    template<typename TT>
    friend class AllocPool;

  private:
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    MPMCStack<T, PreZeroed> stack;
    T* list{nullptr};

  public:
    constexpr PoolState() = default;
  };

  template<typename T, typename SharedStateHandle>
  class Pool
  {
    PoolState<T> state;

  public:
    static Pool* make() noexcept
    {
      return ChunkAllocator::alloc_meta_data<Pool, SharedStateHandle>(nullptr);
    }

    template<typename... Args>
    T* acquire(Args&&... args)
    {
      T* p = state.stack.pop();

      if (p != nullptr)
      {
        p->set_in_use();
        return p;
      }

      p = ChunkAllocator::alloc_meta_data<T, SharedStateHandle>(
        nullptr, std::forward<Args>(args)...);

      FlagLock f(state.lock);
      p->list_next = state.list;
      state.list = p;

      p->set_in_use();
      return p;
    }

    /**
     * Return to the pool an object previously retrieved by `acquire`
     *
     * Do not return objects from `extract`.
     */
    void release(T* p)
    {
      // The object's destructor is not run. If the object is "reallocated", it
      // is returned without the constructor being run, so the object is reused
      // without re-initialisation.
      p->reset_in_use();
      state.stack.push(p);
    }

    T* extract(T* p = nullptr)
    {
      // Returns a linked list of all objects in the stack, emptying the stack.
      if (p == nullptr)
        return state.stack.pop_all();

      return p->next;
    }

    /**
     * Return to the pool a list of object previously retrieved by `extract`
     *
     * Do not return objects from `acquire`.
     */
    void restore(T* first, T* last)
    {
      // Pushes a linked list of objects onto the stack. Use to put a linked
      // list returned by extract back onto the stack.
      state.stack.push(first, last);
    }

    T* iterate(T* p = nullptr)
    {
      if (p == nullptr)
        return state.list;

      return p->list_next;
    }
  };

  /**
   * Collection of static wrappers for the allocator pool.
   * The PoolState for this particular pool type is owned by the
   * SharedStateHandle, so there is no object state in this class.
   */
  template<typename T>
  class AllocPool
  {
  public:
    template<typename SharedStateHandle, typename... Args>
    static T* acquire(Args&&... args)
    {
      PoolState<T>& pool = SharedStateHandle::pool();
      T* p = pool.stack.pop();

      if (p != nullptr)
      {
        p->set_in_use();
        return p;
      }

      p = ChunkAllocator::alloc_meta_data<T, SharedStateHandle>(
        nullptr, std::forward<Args>(args)...);

      if (p == nullptr)
      {
        SharedStateHandle::Pal::error(
          "Failed to initialise thread local allocator.");
      }

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
    static void release(T* p)
    {
      // The object's destructor is not run. If the object is "reallocated", it
      // is returned without the constructor being run, so the object is reused
      // without re-initialisation.
      p->reset_in_use();
      SharedStateHandle::pool().stack.push(p);
    }

    template<typename SharedStateHandle>
    static T* extract(T* p = nullptr)
    {
      // Returns a linked list of all objects in the stack, emptying the stack.
      if (p == nullptr)
        return SharedStateHandle::pool().stack.pop_all();

      return p->next;
    }

    /**
     * Return to the pool a list of object previously retrieved by `extract`
     *
     * Do not return objects from `acquire`.
     */
    template<typename SharedStateHandle>
    static void restore(T* first, T* last)
    {
      // Pushes a linked list of objects onto the stack. Use to put a linked
      // list returned by extract back onto the stack.
      SharedStateHandle::pool().stack.push(first, last);
    }

    template<typename SharedStateHandle>
    static T* iterate(T* p = nullptr)
    {
      if (p == nullptr)
        return SharedStateHandle::pool().list;

      return p->list_next;
    }
  };
} // namespace snmalloc
