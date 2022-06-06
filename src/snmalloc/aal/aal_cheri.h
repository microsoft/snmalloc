#pragma once

#include "../ds_core/ds_core.h"

#include <stddef.h>

namespace snmalloc
{
  /**
   * A mixin AAL that applies CHERI to a `Base` architecture.  Gives
   * architectural teeth to the capptr_bound primitive.
   */
  template<typename Base>
  class AAL_CHERI : public Base
  {
  public:
    /**
     * CHERI pointers are not integers and come with strict provenance
     * requirements.
     */
    static constexpr uint64_t aal_features =
      (Base::aal_features & ~IntegerPointers) | StrictProvenance;

    enum AalCheriFeatures : uint64_t
    {
      /**
       * This CHERI flavor traps if the capability input to a bounds-setting
       * instruction has its tag clear, rather than just leaving the output
       * untagged.
       *
       * For example, CHERI-RISC-V's CSetBoundsExact traps in contrast to
       * Morello's SCBNDSE.
       */
      SetBoundsTrapsUntagged = (1 << 0),

      /**
       * This CHERI flavor traps if the capability input to a
       * permissions-masking instruction has its tag clear, rather than just
       * leaving the output untagged.
       *
       * For example, CHERI-RISC-V's CAndPerms traps in contrast to Morello's
       * CLRPERM.
       */
      AndPermsTrapsUntagged = (1 << 0),
    };

    /**
     * Specify "features" of the particular CHERI machine we're running on.
     */
    static constexpr uint64_t aal_cheri_features =
      /* CHERI-RISC-V prefers to trap on untagged inputs.  Morello does not. */
      (Base::aal_name == RISCV ?
         SetBoundsTrapsUntagged | AndPermsTrapsUntagged :
         0);

    /**
     * On CHERI-aware compilers, ptraddr_t is an integral type that is wide
     * enough to hold any address that may be contained within a memory
     * capability.  It does not carry provenance: it is not a capability, but
     * merely an address.
     */
    typedef ptraddr_t address_t;

    template<
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BOut,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BIn,
      typename U = T>
    static SNMALLOC_FAST_PATH CapPtr<T, BOut>
    capptr_bound(CapPtr<U, BIn> a, size_t size) noexcept
    {
      static_assert(
        BIn::spatial > capptr::dimension::Spatial::Alloc,
        "Refusing to re-bound Spatial::Alloc CapPtr");
      static_assert(
        capptr::is_spatial_refinement<BIn, BOut>(),
        "capptr_bound must preserve non-spatial CapPtr dimensions");
      SNMALLOC_ASSERT(__builtin_cheri_tag_get(a.unsafe_ptr()));

      if constexpr (aal_cheri_features & SetBoundsTrapsUntagged)
      {
        if (a == nullptr)
        {
          return nullptr;
        }
      }

      void* pb = __builtin_cheri_bounds_set_exact(a.unsafe_ptr(), size);
      return CapPtr<T, BOut>::unsafe_from(static_cast<T*>(pb));
    }
  };
} // namespace snmalloc
