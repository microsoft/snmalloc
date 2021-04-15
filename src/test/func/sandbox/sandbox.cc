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
   *
   * CapPtr-vs-MSVC triggering; xref CapPtr's constructor
   */
  bool never_init(void*)
  {
    return false;
  }
  /**
   * Helper for Alloc that never needs lazy initialisation.
   *
   * CapPtr-vs-MSVC triggering; xref CapPtr's constructor
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

    struct ArenaMap
    {
      /**
       * A pointer with authority to the entire sandbox region
       */
      CapPtr<void, CBArena> arena_root;

      /**
       * Amplify using arena_root; that is, exclusively within the sandbox.
       */
      template<
        typename T = void,
        typename U,
        SNMALLOC_CONCEPT(capptr_bounds::c) B>
      SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
      {
        return Aal::capptr_rebound<T>(arena_root, r);
      }

      /*
       * This class does not implement register_root; there should be no
       * attempts to call that function.
       */
    };

    /**
     * The MemoryProvider for sandbox-memory-backed Allocs, both inside and
     * outside the sandbox proper: no memory allocation operations and
     * amplification confined to sandbox memory.
     */
    using NoOpMemoryProvider = MemoryProviderStateMixin<NoOpPal, ArenaMap>;

    /**
     * A NoOpMemoryProvider that additionally dewilds pointers by testing
     * whether they are in the sandbox region.  This will be used by
     * ExternalAlloc-s but not by the InternalAlloc-s, which are presumed never
     * to hold out-of-sandbox memory.
     */
    struct ExternalMemoryProvider : public NoOpMemoryProvider
    {
      const address_t arena_addr;
      const address_t arena_end;

      ExternalMemoryProvider(CapPtr<void, CBChunk> start, size_t len)
      : NoOpMemoryProvider(start, len),
        arena_addr(address_cast(start)),
        arena_end(arena_addr + len)
      {}

      template<typename T>
      SNMALLOC_FAST_PATH CapPtr<T, CBAllocE>
      capptr_dewild(CapPtr<T, CBAllocEW> p)
      {
        address_t a = address_cast(p);
        if ((arena_addr <= a) && (a < arena_end))
          return Aal::capptr_dewild(p);
        else
          return nullptr;
      }
    };

    /**
     * Type for the allocator that lives outside of the sandbox and allocates
     * sandbox-owned memory.
     * This Allocator, by virtue of having its amplification confined to
     * the sandbox, can be used to free only allocations made from sandbox
     * memory.  It (insecurely) routes messages to in-sandbox snmallocs,
     * though, so it can free any sandbox-backed snmalloc allocation.
     */
    using ExternalAlloc = Allocator<
      never_init,
      no_op_init,
      ExternalMemoryProvider,
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
      NoOpMemoryProvider* real_state;

      /**
       * Pop an element from the large stack for the specified size class,
       * proxies to the real implementation.
       *
       * This method must be implemented for `LargeAlloc` to work.
       */
      CapPtr<Largeslab, CBChunk> pop_large_stack(size_t large_class)
      {
        return real_state->pop_large_stack(large_class);
      };

      /**
       * Push an element to the large stack for the specified size class,
       * proxies to the real implementation.
       *
       * This method must be implemented for `LargeAlloc` to work.
       */
      void push_large_stack(CapPtr<Largeslab, CBChunk> slab, size_t large_class)
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
      CapPtr<Largeslab, CBChunk> reserve(size_t large_class) noexcept
      {
        return real_state->template reserve<committed>(large_class);
      }

      /**
       * Amplify by appealing to the real_state, which has our sandbox
       * ArenaMap implementation.
       */
      template<
        typename T = void,
        typename U,
        SNMALLOC_CONCEPT(capptr_bounds::c) B>
      SNMALLOC_FAST_PATH CapPtr<T, CBArena> capptr_amplify(CapPtr<U, B> r)
      {
        return real_state->template capptr_amplify<T>(r);
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
    ExternalMemoryProvider extstate;

    /* extstate, but upcast to change static dispatch */
    NoOpMemoryProvider* state;

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
      extstate(
        pointer_offset(CapPtr<void, CBChunk>(start), sizeof(SharedState)),
        sb_size - sizeof(SharedState)),
      state(&extstate),
      alloc(extstate, SNMALLOC_DEFAULT_CHUNKMAP(), &shared_state->queue)
    {
      // Register the sandbox memory with the sandbox arenamap
      state->arenamap().arena_root = CapPtr<void, CBArena>(start);

      auto* state_proxy = static_cast<MemoryProviderProxy*>(
        alloc.alloc(sizeof(MemoryProviderProxy)));
      state_proxy->real_state = state;
      // In real code, allocators should never be constructed like this, they
      // should always come from an alloc pool.  This is just to test that both
      // kinds of allocator can be created.
      internal_alloc =
        new (alloc.alloc(sizeof(InternalAlloc))) InternalAlloc(*state_proxy);
    }

    Sandbox() = delete;

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
      // Use the outside-sandbox snmalloc to allocate memory, rather than using
      // the PAL directly, so that our out-of-sandbox can amplify sandbox
      // pointers
      return ThreadAlloc::get_noncachable()->alloc(sb_size);
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
