#pragma once

#include "../ds/defines.h"

#include <atomic>

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

  /**
   * Default Tag ID for the Apple class
   */
  static const int PALAnonDefaultID = 241;

  /**
   * This struct is used to represent callbacks for notification from the
   * platform. It contains a next pointer as client is responsible for
   * allocation as we cannot assume an allocator at this point.
   */
  struct PalNotificationObject
  {
    std::atomic<PalNotificationObject*> pal_next;

    void (*pal_notify)(PalNotificationObject* self);
  };

  /***
   * Wrapper for managing notifications for PAL events
   */
  class PalNotifier
  {
    /**
     * List of callbacks to notify
     */
    std::atomic<PalNotificationObject*> callbacks{nullptr};

  public:
    /**
     * Register a callback object to be notified
     *
     * The object should never be deallocated by the client after calling
     * this.
     */
    void register_notification(PalNotificationObject* callback)
    {
      callback->pal_next = nullptr;

      auto prev = &callbacks;
      auto curr = prev->load();
      do
      {
        while (curr != nullptr)
        {
          prev = &(curr->pal_next);
          curr = prev->load();
        }
      } while (!prev->compare_exchange_weak(curr, callback));
    }

    /**
     * Calls the pal_notify of all the registered objects.
     */
    void notify_all()
    {
      PalNotificationObject* curr = callbacks;
      while (curr != nullptr)
      {
        curr->pal_notify(curr);
        curr = curr->pal_next;
      }
    }
  };
} // namespace snmalloc
