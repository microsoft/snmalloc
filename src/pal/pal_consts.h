#pragma once

#include "../ds/defines.h"

#include <atomic>
#include <functional>

namespace snmalloc
{
  /**
   * Pal implementations should query this flag to see whether they
   * are allowed to optimise memory access, or that they must provide
   * exceptions/segfaults if accesses do not obey the
   *  - using
   *  - using_readonly
   *  - not_using
   * model.
   *
   * TODO: There is a known bug in CheriBSD that means round-tripping through
   * PROT_NONE sheds capability load and store permissions (while restoring data
   * read/write, for added excitement).  For the moment, just force this down on
   * CHERI.
   */
#if defined(SNMALLOC_CHECK_CLIENT) && !defined(__CHERI_PURE_CAPABILITY__)
  static constexpr bool PalEnforceAccess = true;
#else
  static constexpr bool PalEnforceAccess = false;
#endif

  /**
   * Flags in a bitfield of optional features that a PAL may support.  These
   * should be set in the PAL's `pal_features` static constexpr field.
   */
  enum PalFeatures : uint64_t
  {
    /**
     * This PAL supports low memory notifications.  It must implement a
     * `register_for_low_memory_callback` method that allows callbacks to be
     * registered when the platform detects low-memory and an
     * `expensive_low_memory_check()` method that returns a `bool` indicating
     * whether low memory conditions are still in effect.
     */
    LowMemoryNotification = (1 << 0),

    /**
     * This PAL natively supports allocation with a guaranteed alignment.  If
     * this is not supported, then we will over-allocate and round the
     * allocation.
     *
     * A PAL that does supports this must expose a `request()` method that takes
     * a size and alignment.  A PAL that does *not* support it must expose a
     * `request()` method that takes only a size.
     */
    AlignedAllocation = (1 << 1),

    /**
     * This PAL natively supports lazy commit of pages. This means have large
     * allocations and not touching them does not increase memory usage. This is
     * exposed in the Pal.
     */
    LazyCommit = (1 << 2),

    /**
     * This Pal does not support allocation.  All memory used with this Pal
     * should be pre-allocated.
     */
    NoAllocation = (1 << 3),

    /**
     * This Pal provides a source of Entropy
     */
    Entropy = (1 << 4),

    /**
     * This Pal provides a millisecond time source
     */
    Time = (1 << 5),
  };

  /**
   * Flag indicating whether requested memory should be zeroed.
   */
  enum ZeroMem
  {
    /**
     * Memory should not be zeroed, contents are undefined.
     */
    NoZero,

    /**
     * Memory must be zeroed.  This can be lazily allocated via a copy-on-write
     * mechanism as long as any load from the memory returns zero.
     */
    YesZero
  };

  enum CoreDump
  {
    /**
     * Default mode, memory are being dumped into a core file
     */
    DoDump,
    /**
     * Memory content not dumped into a core file
     */
    DontDump,
  };

  /**
   * Default Tag ID for the Apple class
   */
  static const int PALAnonDefaultID = 241;

  /**
   * Query whether the PAL supports a specific feature.
   */
  template<PalFeatures F, typename PAL>
  constexpr static bool pal_supports = (PAL::pal_features & F) == F;
} // namespace snmalloc
