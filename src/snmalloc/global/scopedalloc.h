#pragma once
#include "../backend/globalconfig.h"

/**
 * This header requires that Alloc has been defined.
 */

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
  struct ScopedAllocator
  {
    /**
     * The allocator that this wrapper will use.
     */
    Alloc alloc;

    /**
     * Constructor.  Claims an allocator from the global pool
     */
    ScopedAllocator()
    {
      alloc.init();
    };

    /**
     * Copying is not supported, it could easily lead to accidental sharing of
     * allocators.
     */
    ScopedAllocator(const ScopedAllocator&) = delete;

    /**
     * Moving is not supported, though it would be easy to add if there's a use
     * case for it.
     */
    ScopedAllocator(ScopedAllocator&&) = delete;

    /**
     * Copying is not supported, it could easily lead to accidental sharing of
     * allocators.
     */
    ScopedAllocator& operator=(const ScopedAllocator&) = delete;

    /**
     * Moving is not supported, though it would be easy to add if there's a use
     * case for it.
     */
    ScopedAllocator& operator=(ScopedAllocator&&) = delete;

    /**
     * Destructor.  Returns the allocator to the pool.
     */
    ~ScopedAllocator()
    {
      alloc.flush();
    }

    /**
     * Arrow operator, allows methods exposed by `Alloc` to be called on the
     * wrapper.
     */
    Alloc* operator->()
    {
      return &alloc;
    }
  };

  /**
   * Returns a new scoped allocator.  When the `ScopedAllocator` goes out of
   * scope, the underlying `Alloc` will be returned to the pool.
   */
  inline ScopedAllocator get_scoped_allocator()
  {
    return {};
  }
} // namespace snmalloc
