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
      // static_assert(
      //   BIn::spatial > capptr::dimension::Spatial::Alloc,
      //   "Refusing to re-bound Spatial::Alloc CapPtr");
      // static_assert(
      //   capptr::is_spatial_refinement<BIn, BOut>(),
      //   "capptr_bound must preserve non-spatial CapPtr dimensions");
      SNMALLOC_ASSERT(__builtin_cheri_tag_get(a.unsafe_ptr()));

      if constexpr (aal_cheri_features & SetBoundsTrapsUntagged)
      {
        if (a == nullptr)
        {
          return nullptr;
        }
      }

      void* pb = __builtin_cheri_bounds_set_exact(a.unsafe_ptr(), size);
      return CapPtr<T, BOut>(static_cast<T*>(pb));
    }
  };

   /**
   * A mixin AAL that applies CHERI Tints to a `Base` architecture.  Implements
   * the tint manipulation primitives.
   */
  template<typename Base>
  class AAL_Tints : public Base
  {
  public:
    /**
     * Add Tints to set of supported features.
     */
    static constexpr uint64_t aal_features = Base::aal_features | Tints;

  public:
    template<
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BIn>
    static SNMALLOC_FAST_PATH tint_t
    capptr_tint_get(CapPtr<T, BIn> a) noexcept
    {
      return static_cast<tint_t>(cheri_getversion(a.unsafe_ptr()));
    }

    template<
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BOut,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BIn,
      typename U = T>
    static SNMALLOC_FAST_PATH CapPtr<T, BOut>
    capptr_tint_set(CapPtr<U, BIn> a, tint_t t) noexcept
    {
      static_assert(
        BIn::tint == capptr::dimension::Tint::Rainbow,
        "Setting tint is only permitted on rainbow pointers");
      static_assert(
        BOut::tint == capptr::dimension::Tint::Monochrome,
        "Setting tint produces a monochrome pointer");
      // XXX assert non-tint dimensions unchanged?
      void* pt = cheri_setversion(a.unsafe_ptr(), t);
      return CapPtr<T, BOut>(static_cast<T*>(pt));
    }

    template<
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BIn>
    static SNMALLOC_FAST_PATH tint_t
    capptr_tint_load(CapPtr<T, BIn> a) noexcept
    {
      static_assert(
        BIn::tint == capptr::dimension::Tint::Rainbow,
        "Only rainbow pointers may be used to load tint");
      return static_cast<tint_t>(cheri_loadversion(a.unsafe_ptr()));
    }

    template<
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BIn>
    static SNMALLOC_FAST_PATH void
    capptr_tint_store(CapPtr<T, BIn> a, tint_t t) noexcept
    {
      static_assert(
        BIn::tint == capptr::dimension::Tint::Rainbow,
        "Only rainbow pointers may be used to store tint");
        cheri_storeversion(a.unsafe_ptr(), t);
    }

    template<
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BAuth,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BExp,
      typename U = T>
    static SNMALLOC_FAST_PATH AmoDecResult
    capptr_tint_amo_dec(CapPtr<T, BAuth> a, CapPtr<U, BExp> te) noexcept
    {
      static_assert(
        BAuth::tint == capptr::dimension::Tint::Rainbow,
        "AMO Dec requires Rainbow pointer for authorisation");
      // static_assert(
      //   BExp::tint == capptr::dimension::Tint::Gray,
      //   "AMO Dec requires Gray pointer for expected tint");
      UNUSED(a);
      UNUSED(te);
      int r = cheri_camocdecversion(a.unsafe_ptr(), te.unsafe_ptr());
      return static_cast<AmoDecResult> (r);
    }
  };
} // namespace snmalloc
