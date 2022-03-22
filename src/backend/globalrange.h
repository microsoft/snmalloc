#pragma once

#include "../ds/defines.h"
#include "../ds/helpers.h"
#include "../ds/ptrwrap.h"

namespace snmalloc
{
  /**
   * Makes the supplied ParentRange into a global variable,
   * and protects access with a lock.
   */
  template<SNMALLOC_CONCEPT(ConceptBackendRange_Alloc) ParentRange>
  class GlobalRange
  {
    typename ParentRange::State parent{};

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    FlagWord spin_lock{};

  public:
    class State
    {
      SNMALLOC_REQUIRE_CONSTINIT static inline GlobalRange global_range{};

    public:
      constexpr GlobalRange* operator->()
      {
        return &global_range;
      }

      constexpr State() = default;
    };

    using B = typename ParentRange::B;

    static constexpr bool Aligned = ParentRange::Aligned;

    constexpr GlobalRange() = default;

    CapPtr<void, B> alloc_range(size_t size)
    {
      FlagLock lock(spin_lock);
      return parent->alloc_range(size);
    }

    template<SNMALLOC_CONCEPT(ConceptBackendRange_Dealloc) _pr = ParentRange>
    void dealloc_range(CapPtr<void, B> base, size_t size)
    {
      static_assert(
        std::is_same<_pr, ParentRange>::value,
        "Don't set SFINAE template parameter!");
      FlagLock lock(spin_lock);
      parent->dealloc_range(base, size);
    }
  };
} // namespace snmalloc
