#pragma once

#include "../backend/backend.h"
#include "../mem/corealloc.h"
#include "../mem/pool.h"
#include "commonconfig.h"

namespace snmalloc
{
  /**
   * A single fixed address range allocator configuration
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL>
  class FixedGlobals final : public BackendAllocator<PAL, true>
  {
  public:
    using GlobalPoolState = PoolState<CoreAllocator<FixedGlobals>>;

  private:
    using Backend = BackendAllocator<PAL, true>;

    inline static GlobalPoolState alloc_pool;

  public:
    static GlobalPoolState& pool()
    {
      return alloc_pool;
    }

    static constexpr Flags Options{};

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
