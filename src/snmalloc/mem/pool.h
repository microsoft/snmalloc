#pragma once

#include "../ds/ds.h"
#include "pooled.h"

#include <new>

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
    template<
      typename TT,
      SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle,
      PoolState<TT>& get_state()>
    friend class Pool;

  private:
    MPMCStack<T, PreZeroed> stack;
    FlagWord lock{};
    T* list{nullptr};
    size_t count{0};

  public:
    constexpr PoolState() = default;

    size_t get_count()
    {
      return count;
    }
  };

  /**
   * Helper class used to instantiate a global PoolState.
   *
   * SingletonPoolState::pool is the default provider for the PoolState within
   * the Pool class.
   */
  template<
    typename T,
    SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
  class SingletonPoolState
  {
    /**
     * SFINAE helper.  Matched only if `T` implements `ensure_init`.  Calls it
     * if it exists.
     */
    template<typename SharedStateHandle_>
    SNMALLOC_FAST_PATH static auto call_ensure_init(SharedStateHandle_*, int)
      -> decltype(SharedStateHandle_::ensure_init())
    {
      static_assert(
        std::is_same<SharedStateHandle, SharedStateHandle_>::value,
        "SFINAE parameter, should only be used with SharedStateHandle");
      SharedStateHandle_::ensure_init();
    }

    /**
     * SFINAE helper.  Matched only if `T` does not implement `ensure_init`.
     * Does nothing if called.
     */
    template<typename SharedStateHandle_>
    SNMALLOC_FAST_PATH static auto call_ensure_init(SharedStateHandle_*, long)
    {
      static_assert(
        std::is_same<SharedStateHandle, SharedStateHandle_>::value,
        "SFINAE parameter, should only be used with SharedStateHandle");
    }

    /**
     * Call `SharedStateHandle::ensure_init()` if it is implemented, do nothing
     * otherwise.
     */
    SNMALLOC_FAST_PATH static void ensure_init()
    {
      call_ensure_init<SharedStateHandle>(nullptr, 0);
    }

    static void make_pool(PoolState<T>*) noexcept
    {
      ensure_init();
      // Default initializer already called on PoolState, no need to use
      // placement new.
    }

  public:
    /**
     * Returns a reference for the global PoolState for the given type.
     * Also forces the initialization of the backend state, if needed.
     */
    SNMALLOC_FAST_PATH static PoolState<T>& pool()
    {
      return Singleton<PoolState<T>, &make_pool>::get();
    }
  };

  /**
   * Wrapper class to access a pool of a particular type of object.
   *
   * The third template argument is a method to retrieve the actual PoolState.
   *
   * For the pool of allocators, refer to the AllocPool alias defined in
   * corealloc.h.
   *
   * For a pool of another type, it is recommended to leave the
   * third template argument with its default value. The SingletonPoolState
   * class is used as a helper to provide a default PoolState management for
   * this use case.
   */
  template<
    typename T,
    SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle,
    PoolState<T>& get_state() = SingletonPoolState<T, SharedStateHandle>::pool>
  class Pool
  {
  public:
    template<typename... Args>
    static T* acquire(Args&&... args)
    {
      PoolState<T>& pool = get_state();
      T* p = pool.stack.pop();

      if (p != nullptr)
      {
        p->set_in_use();
        return p;
      }

      auto raw =
        SharedStateHandle::template alloc_meta_data<T>(nullptr, sizeof(T));

      if (raw == nullptr)
      {
        SharedStateHandle::Pal::error(
          "Failed to initialise thread local allocator.");
      }

      p = new (raw.unsafe_ptr()) T(std::forward<Args>(args)...);

      FlagLock f(pool.lock);
      p->list_next = pool.list;
      pool.list = p;

      pool.count++;

      p->set_in_use();
      return p;
    }

    /**
     * Return to the pool an object previously retrieved by `acquire`
     *
     * Do not return objects from `extract`.
     */
    static void release(T* p)
    {
      // The object's destructor is not run. If the object is "reallocated", it
      // is returned without the constructor being run, so the object is reused
      // without re-initialisation.
      p->reset_in_use();
      get_state().stack.push(p);
    }

    static T* extract(T* p = nullptr)
    {
      // Returns a linked list of all objects in the stack, emptying the stack.
      if (p == nullptr)
        return get_state().stack.pop_all();

      return p->next;
    }

    /**
     * Return to the pool a list of object previously retrieved by `extract`
     *
     * Do not return objects from `acquire`.
     */
    static void restore(T* first, T* last)
    {
      // Pushes a linked list of objects onto the stack. Use to put a linked
      // list returned by extract back onto the stack.
      get_state().stack.push(first, last);
    }

    static T* iterate(T* p = nullptr)
    {
      if (p == nullptr)
        return get_state().list;

      return p->list_next;
    }
  };
} // namespace snmalloc
