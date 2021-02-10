#pragma once

#include "../ds/defines.h"
#include "../ds/helpers.h"

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

  template <typename T>
  class PalList
  {
    /**
     * List of callbacks to notify
     */
    std::atomic<T*> elements{nullptr};

  public:
    /**
     * Add an element to the list
     */
    void add(T* element)
    {
      callback->pal_next = nullptr;

      auto prev = &elements;
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
     * Applies function to all the elements of the list
     */
    void apply_all(function_ref<void(T*)> func)
    {
      T* curr = elements;
      while (curr != nullptr)
      {
        func(curr);
        curr = curr->pal_next;
      }
    }
  };

  /**
   * This struct is used to represent callbacks for notification from the
   * platform. It contains a next pointer as client is responsible for
   * allocation as we cannot assume an allocator at this point.
   */
  struct PalNotificationObject
  {
    std::atomic<PalNotificationObject*> pal_next = nullptr;

    void (*pal_notify)(PalNotificationObject* self);

    PalNotificationObject(void (*pal_notify)(PalNotificationObject* self))
    : pal_notify(pal_notify)
    {}
  };

  /***
   * Wrapper for managing notifications for PAL events
   */
  class PalNotifier
  {
    /**
     * List of callbacks to notify
     */
    PalList<PalNotificationObject> callbacks;

  public:
    /**
     * Register a callback object to be notified
     *
     * The object should never be deallocated by the client after calling
     * this.
     */
    void register_notification(PalNotificationObject* callback)
    {
      callbacks.add(callback);
    }

    /**
     * Calls the pal_notify of all the registered objects.
     */
    void notify_all()
    {
      callbacks.apply_all([](auto curr){curr->pal_notify(curr);});
    }
  };

  struct PalTimerObject
  {
    std::atomic<PalTimerObject*> pal_next;

    void (*pal_notify)(PalTimerObject* self);

    uint64_t last_run = 0;
    uint64_t repeat;
  };

  /**
   * Simple mechanism for handling timers.
   * 
   * Note: This is really designed for a very small number of timers, 
   * and this design should be changed if that is no longer the case.
   */
  class PalTimer
  {
    /**
     * List of callbacks to notify
     */
    PalList<PalTimerObject> timers;

    /**
     * Register a callback to be called every repeat milliseconds.
     */
    void register_timer(PalTimerObject* timer)
    {
      timers.add(timer);
    }

    void check(uint64_t time_ms)
    {
      timers.apply_all([time_ms](PalTimerObject* curr){
        if ((curr->last_run == 0)
          || ((time_ms - curr->last_run) > curr->repeat))
        {
          curr->last_run = time_ms;
          curr->pal_notify(curr);
        }
      });
    }
  };
} // namespace snmalloc
