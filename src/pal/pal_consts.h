#pragma once

namespace snmalloc
{
  /**
   * Flags in a bitfield of optional features that a PAL may support.  These
   * should be set in the PAL's `pal_features` static constexpr field.
   */
  enum PalFeatures : uint64_t
  {
    /**
     * This PAL supports low memory notifications.  It must implement a
     * `low_memory_epoch` method that returns a `uint64_t` of the number of
     * times that a low-memory notification has been raised and an
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
    AlignedAllocation = (1 << 1)
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
} // namespace snmalloc
