#pragma once

#include "../ds/flaglock.h"
#include "../ds/mpmcstack.h"
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
   **/
  template<class T, class MemoryProvider = GlobalVirtual>
  class Pool
  {
  private:
    friend Pooled<T>;
    template<typename TT>
    friend class MemoryProviderStateMixin;

    std::atomic_flag lock = ATOMIC_FLAG_INIT;
    MPMCStack<T, PreZeroed> stack;
    T* list = nullptr;

    Pool(MemoryProvider& m) : memory_provider(m) {}

  public:
    MemoryProvider& memory_provider;

    static Pool* make(MemoryProvider& memory_provider) noexcept
    {
      return memory_provider.template alloc_chunk<Pool, 0, MemoryProvider&>(
        memory_provider);
    }

    static Pool* make() noexcept
    {
      return make(default_memory_provider);
    }

    template<typename... Args>
    T* acquire(Args&&... args)
    {
      T* p = stack.pop();

      if (p != nullptr)
        return p;

      p = memory_provider
            .template alloc_chunk<T, bits::next_pow2_const(sizeof(T))>(
              std::forward<Args...>(args)...);

      FlagLock f(lock);
      p->list_next = list;
      list = p;

      return p;
    }

    void release(T* p)
    {
      // The object's destructor is not run. If the object is "reallocated", it
      // is returned without the constructor being run, so the object is reused
      // without re-initialisation.
      stack.push(p);
    }

    T* extract(T* p = nullptr)
    {
      // Returns a linked list of all objects in the stack, emptying the stack.
      if (p == nullptr)
        return stack.pop_all();
      else
        return p->next;
    }

    void restore(T* first, T* last)
    {
      // Pushes a linked list of objects onto the stack. Use to put a linked
      // list returned by extract back onto the stack.
      stack.push(first, last);
    }

    T* iterate(T* p = nullptr)
    {
      if (p == nullptr)
        return list;
      else
        return p->list_next;
    }
  };
}
