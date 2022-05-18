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
  public:
    using GlobalPoolState = PoolState<CoreAllocator<FixedGlobals>>;

    using Backend = BackendAllocator<PAL, true, PageMapEntry>;
    using Pal = Pal;
    using Pagemap = typename Backend::Pagemap;
    using LocalState = typename Backend::LocalState;
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
    init(typename Backend::LocalState* local_state, void* base, size_t length)
    {
      UNUSED(local_state);
      Backend::init(base, length);
    }

    /* Verify that a pointer points into the region managed by this config */
    template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    static SNMALLOC_FAST_PATH CapPtr<
      T,
      typename B::template with_wildness<capptr::dimension::Wildness::Tame>>
    capptr_domesticate(typename Backend::LocalState* ls, CapPtr<T, B> p)
    {
      static_assert(B::wildness == capptr::dimension::Wildness::Wild);

      static const size_t sz = sizeof(
        std::conditional<std::is_same_v<std::remove_cv<T>, void>, void*, T>);

      UNUSED(ls);
      auto address = address_cast(p);
      auto [base, length] = Backend::Pagemap::get_bounds();
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
