#pragma once

#if defined(__FreeBSD__) && !defined(_KERNEL)
#  include "pal_bsd_aligned.h"

namespace snmalloc
{
  /**
   * FreeBSD-specific platform abstraction layer.
   *
   * This adds FreeBSD-specific aligned allocation to the generic BSD
   * implementation.
   */
  class PALFreeBSD : public PALBSD_Aligned<PALFreeBSD>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     *
     * The FreeBSD PAL does not currently add any features beyond those of a
     * generic BSD with support for arbitrary alignment from `mmap`.  This
     * field is declared explicitly to remind anyone modifying this class to
     * add new features that they should add any required feature flags.
     */
    static constexpr uint64_t pal_features = PALBSD_Aligned::pal_features;

    /**
     * FreeBSD uses atypically small address spaces on its 64 bit RISC machines.
     * Problematically, these are so small that if we used the default
     * address_bits (48), we'd try to allocate the whole AS (or larger!) for the
     * Pagemap itself!
     */
    static constexpr size_t address_bits = (Aal::bits == 32) ?
      Aal::address_bits :
      (Aal::aal_name == RISCV ? 38 : Aal::address_bits);
    // TODO, if we ever backport to MIPS, this should yield 39 there.
  };
} // namespace snmalloc
#endif
