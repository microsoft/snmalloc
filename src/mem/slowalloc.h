#include "globalalloc.h"

namespace snmalloc
{
  /**
   * RAII wrapper around an `Alloc`.  This class gets an allocator from the
   * global pool and wraps it so that `Alloc` methods can be called
   * directly via the `->` operator on this class.  When this object is
   * destroyed, it returns the allocator to the global pool.
   *
   * This does not depend on thread-local storage working, so can be used for
   * bootstrapping.
   */
  struct SlowAllocator
  {
    /**
     * The allocator that this wrapper will use.
     */
    Alloc* alloc;
    /**
     * Constructor.  Claims an allocator from the global pool
     */
    SlowAllocator() : alloc(current_alloc_pool()->acquire()) {}
    /**
     * Copying is not supported, it could easily lead to accidental sharing of
     * allocators.
     */
    SlowAllocator(const SlowAllocator&) = delete;
    /**
     * Moving is not supported, though it would be easy to add if there's a use
     * case for it.
     */
    SlowAllocator(SlowAllocator&&) = delete;
    /**
     * Copying is not supported, it could easily lead to accidental sharing of
     * allocators.
     */
    SlowAllocator& operator=(const SlowAllocator&) = delete;
    /**
     * Moving is not supported, though it would be easy to add if there's a use
     * case for it.
     */
    SlowAllocator& operator=(SlowAllocator&&) = delete;
    /**
     * Destructor.  Returns the allocator to the pool.
     */
    ~SlowAllocator()
    {
      current_alloc_pool()->release(alloc);
    }
    /**
     * Arrow operator, allows methods exposed by `Alloc` to be called on the
     * wrapper.
     */
    Alloc* operator->()
    {
      return alloc;
    }
  };
  /**
   * Returns a new slow allocator.  When the `SlowAllocator` goes out of scope,
   * the underlying `Alloc` will be returned to the pool.
   */
  inline SlowAllocator get_slow_allocator()
  {
    return {};
  }
} // namespace snmalloc
