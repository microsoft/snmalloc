#pragma once

#if defined(__FreeBSD__) && !defined(_KERNEL)
#  include "pal_bsd_aligned.h"

// On CHERI platforms, we need to know the value of CHERI_PERM_SW_VMEM.
// This pollutes the global namespace a little, sadly, but I think only with
// symbols that begin with CHERI_, which is as close to namespaces as C offers.
#  if defined(__CHERI_PURE_CAPABILITY__)
#    include <cheri/cherireg.h>
#    if !defined(CHERI_PERM_SW_VMEM)
#      define CHERI_PERM_SW_VMEM CHERI_PERM_CHERIABI_VMMAP
#    endif
#  endif

/**
 * Direct system-call wrappers so that we can skip libthr interception, which
 * won't work if malloc is broken.
 * @{
 */
extern "C" ssize_t __sys_writev(int fd, const struct iovec* iov, int iovcnt);
extern "C" int __sys_fsync(int fd);
/// @}

namespace snmalloc
{
  /**
   * FreeBSD-specific platform abstraction layer.
   *
   * This adds FreeBSD-specific aligned allocation to the generic BSD
   * implementation.
   */
  class PALFreeBSD
  : public PALBSD_Aligned<PALFreeBSD, __sys_writev, __sys_fsync>
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

    /**
     * Extra mmap flags.  Exclude mappings from core files if they are
     * read-only or pure reservations.
     */
    static int extra_mmap_flags(bool state_using)
    {
      return state_using ? 0 : MAP_NOCORE;
    }

    /**
     * Notify platform that we will not be using these pages.
     *
     * We use the `MADV_FREE` and `NADV_NOCORE` flags to `madvise`.  The first
     * allows the system to discard the page and replace it with a CoW mapping
     * of the zero page.  The second prevents this mapping from appearing in
     * core files.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

      if constexpr (DEBUG)
        memset(p, 0x5a, size);

      madvise(p, size, MADV_NOCORE);
      madvise(p, size, MADV_FREE);

      if constexpr (mitigations(pal_enforce_access))
      {
        mprotect(p, size, PROT_NONE);
      }
    }

    /**
     * Notify platform that we will be using these pages for reading.
     *
     * This is used only for pages full of zeroes and so we exclude them from
     * core dumps.
     */
    static void notify_using_readonly(void* p, size_t size) noexcept
    {
      PALBSD_Aligned<PALFreeBSD>::notify_using_readonly(p, size);
      madvise(p, size, MADV_NOCORE);
    }

    /**
     * Notify platform that we will be using these pages.
     *
     * We may have previously marked this memory as not being included in core
     * files, so mark it for inclusion again.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      PALBSD_Aligned<PALFreeBSD>::notify_using<zero_mem>(p, size);
      madvise(p, size, MADV_CORE);
    }

#  if defined(__CHERI_PURE_CAPABILITY__)
    static_assert(
      aal_supports<StrictProvenance>,
      "CHERI purecap support requires StrictProvenance AAL");

    /**
     * On CheriBSD, exporting a pointer means stripping it of the authority to
     * manage the address space it references by clearing the SW_VMEM
     * permission bit.
     */
    template<typename T, SNMALLOC_CONCEPT(capptr::IsBound) B>
    static SNMALLOC_FAST_PATH CapPtr<T, capptr::user_address_control_type<B>>
    capptr_to_user_address_control(CapPtr<T, B> p)
    {
      if constexpr (Aal::aal_cheri_features & Aal::AndPermsTrapsUntagged)
      {
        if (p == nullptr)
        {
          return nullptr;
        }
      }
      return CapPtr<T, capptr::user_address_control_type<B>>::unsafe_from(
        __builtin_cheri_perms_and(
          p.unsafe_ptr(), ~static_cast<unsigned int>(CHERI_PERM_SW_VMEM)));
    }
#  endif
  };
} // namespace snmalloc
#endif
