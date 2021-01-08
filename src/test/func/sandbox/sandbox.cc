#ifdef SNMALLOC_PASS_THROUGH
/*
 * This test does not make sense with malloc pass-through, skip it.
 */
int main()
{
  return 0;
}
#else
// The decommit strategy is currently a global policy and not per-allocator and
// so we need to tell Windows not to use the lazy strategy for this test.
#  define USE_DECOMMIT_STRATEGY DecommitSuper
#  include <snmalloc.h>

using namespace snmalloc;

namespace
{
  /**
   * Helper for Alloc that is never used as a thread-local allocator and so is
   * always initialised.
   */
  bool never_init(void*)
  {
    return false;
  }
  /**
   * Helper for Alloc that never needs lazy initialisation.
   */
  void* no_op_init(function_ref<void*(void*)>)
  {
    SNMALLOC_CHECK(0 && "Should never be called!");
    return nullptr;
  }
  /**
   * Sandbox class.  Allocates a memory region and an allocator that can
   * allocate into this from the outside.
   */
  struct Sandbox
  {
    using NoOpPal = PALNoAlloc<DefaultPal>;
    /**
     * Type for the allocator that lives outside of the sandbox and allocates
     * sandbox-owned memory.
     */
    using ExternalAlloc = Allocator<
      never_init,
      no_op_init,
      MemoryProviderStateMixin<NoOpPal>,
      SNMALLOC_DEFAULT_CHUNKMAP,
      false>;
    /**
     * Proxy class that forwards requests for large allocations to the real
     * memory provider.
     *
     * In a real implementation, these would be cross-domain calls with the
     * callee verifying the arguments.
     */
    struct MemoryProviderProxy
    {
      /**
       * The PAL that allocators using this memory provider should use.
       */
      typedef NoOpPal Pal;
      /**
       * The pointer to the real state.  In a real implementation there would
       * likely be only one of these inside any given sandbox and so this would
       * not have to be per-instance state.
       */
      MemoryProviderStateMixin<NoOpPal>* real_state;

      /**
       * Pop an element from the large stack for the specified size class,
       * proxies to the real implementation.
       *
       * This method must be implemented for `LargeAlloc` to work.
       */
      void* pop_large_stack(size_t large_class)
      {
        return real_state->pop_large_stack(large_class);
      };

      /**
       * Push an element to the large stack for the specified size class,
       * proxies to the real implementation.
       *
       * This method must be implemented for `LargeAlloc` to work.
       */
      void push_large_stack(Largeslab* slab, size_t large_class)
      {
        real_state->push_large_stack(slab, large_class);
      }

      /**
       * Reserve (and optionally commit) memory for a large sizeclass, proxies
       * to the real implementation.
       *
       * This method must be implemented for `LargeAlloc` to work.
       */
      template<bool committed>
      void* reserve(size_t large_class) noexcept
      {
        return real_state->template reserve<committed>(large_class);
      }
    };

    /**
     * Type for the allocator that exists inside the sandbox.
     *
     * Note that a real version of this would not have access to the shared
     * pagemap and would not be used outside of the sandbox.
     */
    using InternalAlloc =
      Allocator<never_init, no_op_init, MemoryProviderProxy>;

    /**
     * The start of the sandbox memory region.
     */
    void* start;

    /**
     * The end of the sandbox memory region
     */
    void* top;

    /**
     * State allocated in the sandbox that is shared between the inside and
     * outside.
     */
    struct SharedState
    {
      /**
       * The message queue for the allocator that lives outside of the
       * sandbox but allocates memory inside.
       */
      struct RemoteAllocator queue;
    } * shared_state;

    /**
     * The memory provider for this sandbox.
     */
    MemoryProviderStateMixin<NoOpPal> state;

    /**
     * The allocator for callers outside the sandbox to allocate memory inside.
     */
    ExternalAlloc alloc;

    /**
     * An allocator for callers inside the sandbox to allocate memory.
     */
    InternalAlloc* internal_alloc;

    /**
     * Constructor.  Takes the size of the sandbox as the argument.
     */
    Sandbox(size_t sb_size)
    : start(alloc_sandbox_heap(sb_size)),
      top(pointer_offset(start, sb_size)),
      shared_state(new (start) SharedState()),
      state(
        pointer_offset(start, sizeof(SharedState)),
        sb_size - sizeof(SharedState)),
      alloc(state, SNMALLOC_DEFAULT_CHUNKMAP(), &shared_state->queue)
    {
      auto* state_proxy = static_cast<MemoryProviderProxy*>(
        alloc.alloc(sizeof(MemoryProviderProxy)));
      state_proxy->real_state = &state;
      // In real code, allocators should never be constructed like this, they
      // should always come from an alloc pool.  This is just to test that both
      // kinds of allocator can be created.
      internal_alloc =
        new (alloc.alloc(sizeof(InternalAlloc))) InternalAlloc(*state_proxy);
    }

    Sandbox() = delete;

    /**
     * Predicate function for querying whether an object is entirely within the
     * sandbox.
     */
    bool is_in_sandbox(void* ptr, size_t sz)
    {
      return (ptr >= start) && (pointer_offset(ptr, sz) < top);
    }

    /**
     * Predicate function for querying whether an object is entirely within the
     * region of the sandbox allocated for its heap.
     */
    bool is_in_sandbox_heap(void* ptr, size_t sz)
    {
      return (
        ptr >= pointer_offset(start, sizeof(SharedState)) &&
        (pointer_offset(ptr, sz) < top));
    }

  private:
    template<typename PAL = DefaultPal>
    void* alloc_sandbox_heap(size_t sb_size)
    {
      if constexpr (pal_supports<AlignedAllocation, PAL>)
      {
        return PAL::template reserve_aligned<true>(sb_size);
      }
      else
      {
        // Note: This wastes address space because the PAL will reserve
        // double the amount we ask for to ensure alignment.  It's fine for
        // the test, but any call to this function that ignores `.second`
        // (the allocated size) is deeply suspect.
        void* ptr = PAL::reserve_at_least(sb_size).first;
        PAL::template notify_using<YesZero>(ptr, sb_size);
        return ptr;
      }
    }
  };
}

int main()
{
  static const size_t sb_size = 128 * 1024 * 1024;

  // Check that we can create two sandboxes
  Sandbox sb1(sb_size);
  Sandbox sb2(sb_size);

  auto check = [](Sandbox& sb, auto& alloc, size_t sz) {
    void* ptr = alloc.alloc(sz);
    SNMALLOC_CHECK(sb.is_in_sandbox_heap(ptr, sz));
    ThreadAlloc::get_noncachable()->dealloc(ptr);
  };
  auto check_with_sb = [&](Sandbox& sb) {
    // Check with a range of sizes
    check(sb, sb.alloc, 32);
    check(sb, *sb.internal_alloc, 32);
    check(sb, sb.alloc, 240);
    check(sb, *sb.internal_alloc, 240);
    check(sb, sb.alloc, 513);
    check(sb, *sb.internal_alloc, 513);
    check(sb, sb.alloc, 10240);
    check(sb, *sb.internal_alloc, 10240);
  };
  check_with_sb(sb1);
  check_with_sb(sb2);

  return 0;
}
#endif
