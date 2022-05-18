#pragma once

#include "../backend/backend.h"

namespace snmalloc
{
  /**
   * A single fixed address range allocator configuration
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class FixedGlobals final : public CommonConfig
  {
    using ConcretePagemap =
      FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, PAL, true>;

    using Pagemap =
      BasicPagemap<PAL, ConcretePagemap, PageMapEntry, true>;

    // Global range of memory
    using GlobalR = Pipe<
      EmptyRange,
      LargeBuddyRange<24, bits::BITS - 1, Pagemap>,
      LogRange<2>,
      GlobalRange<>>;

    // Source for object allocations and metadata
    // No separation between the two
    using Stats = Pipe<GlobalR, StatsRange<>>;
    using ObjectRange = Pipe<
      Stats,
      CommitRange<PAL>,
      LargeBuddyRange<21, 21, Pagemap>,
      SmallBuddyRange<>>;
    using GlobalMetaRange = Pipe<ObjectRange, GlobalRange<>>;

  public:
    struct LocalState
    {
      ObjectRange object_range;

      ObjectRange& get_meta_range()
      {
        return object_range;
      }
    };

    using GlobalPoolState = PoolState<CoreAllocator<FixedGlobals>>;
      
    using Backend = BackendAllocator<PAL, PageMapEntry, Pagemap, LocalState, GlobalMetaRange, Stats>;
    using Pal = PAL;
    using SlabMetadata = typename Backend::SlabMetadata;

  private:
    inline static GlobalPoolState alloc_pool;

  public:
    static GlobalPoolState& pool()
    {
      return alloc_pool;
    }

    /*
     * The obvious
     * `static constexpr Flags Options{.HasDomesticate = true};` fails on
     * Ubuntu 18.04 with an error "sorry, unimplemented: non-trivial
     * designated initializers not supported".
     * The following was copied from domestication.cc test with the following
     * comment:
     * C++, even as late as C++20, has some really quite strict limitations on
     * designated initializers.  However, as of C++17, we can have constexpr
     * lambdas and so can use more of the power of the statement fragment of
     * C++, and not just its initializer fragment, to initialize a non-prefix
     * subset of the flags (in any order, at that).
     */
    static constexpr Flags Options = []() constexpr
    {
      Flags opts = {};
      opts.HasDomesticate = true;
      return opts;
    }
    ();

    // This needs to be a forward reference as the
    // thread local state will need to know about this.
    // This may allocate, so must be called once a thread
    // local allocator exists.
    static void register_clean_up()
    {
      snmalloc::register_clean_up();
    }

    static void
    init(LocalState* local_state, void* base, size_t length)
    {
      UNUSED(local_state);

      auto [heap_base, heap_length] =
        Pagemap::concretePagemap.init(base, length);

      Pagemap::register_range(address_cast(heap_base), heap_length);

      // Push memory into the global range.
      range_to_pow_2_blocks<MIN_CHUNK_BITS>(
        capptr::Chunk<void>(heap_base),
        heap_length,
        [&](capptr::Chunk<void> p, size_t sz, bool) {
          GlobalR g;
          g.dealloc_range(p, sz);
        });
    }

    /* Verify that a pointer points into the region managed by this config */
    template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    static SNMALLOC_FAST_PATH CapPtr<
      T,
      typename B::template with_wildness<capptr::dimension::Wildness::Tame>>
    capptr_domesticate(LocalState* ls, CapPtr<T, B> p)
    {
      static_assert(B::wildness == capptr::dimension::Wildness::Wild);

      static const size_t sz = sizeof(
        std::conditional<std::is_same_v<std::remove_cv<T>, void>, void*, T>);

      UNUSED(ls);
      auto address = address_cast(p);
      auto [base, length] = Pagemap::get_bounds();
      if ((address - base > (length - sz)) || (length < sz))
      {
        return nullptr;
      }

      return CapPtr<
        T,
        typename B::template with_wildness<capptr::dimension::Wildness::Tame>>(
        p.unsafe_ptr());
    }
  };
}
