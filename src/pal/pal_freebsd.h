#pragma once

#if defined(__FreeBSD__) && !defined(_KERNEL)
#  include "pal_bsd_aligned.h"

// On CHERI platforms, we need to know the value of CHERI_PERM_CHERIABI_VMMAP.
// This pollutes the global namespace a little, sadly, but I think only with
// symbols that begin with CHERI_, which is as close to namespaces as C offers.
#  if defined(__CHERI_PURE_CAPABILITY__)
#    include <cheri/cherireg.h>
#  endif

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

#  if defined(__CHERI_PURE_CAPABILITY__)
    static_assert(
      aal_supports<StrictProvenance>,
      "CHERI purecap support requires StrictProvenance AAL");

    /**
     * On CheriBSD, exporting a pointer means stripping it of the authority to
     * manage the address space it references by clearing the CHERIABI_VMMAP
     * permission bit.
     */
    template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    static SNMALLOC_FAST_PATH CapPtr<T, capptr::user_address_control_type<B>>
    capptr_to_user_address_control(CapPtr<T, B> p)
    {
      return CapPtr<T, capptr::user_address_control_type<B>>(
        __builtin_cheri_perms_and(
          p.unsafe_ptr(),
          ~static_cast<unsigned int>(CHERI_PERM_CHERIABI_VMMAP)));
    }
#  endif

    static void nodump(void* p, size_t size) noexcept
    {
      madvise(p, size, MADV_NOCORE);
    }
  };
} // namespace snmalloc
#endif
