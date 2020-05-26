#pragma once

#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"
#include "../mem/sizeclass.h"

#include <atomic>

namespace snmalloc
{
  /*
   * A region of memory destined for a remote allocator's dealloc() via the
   * message passing system.  This structure is placed at the beginning of
   * the allocation itself when it is queued for sending.
   *
   * A Remote* is itself a ReturnPtr; that is, for architectures supporting
   * StrictProvenance, this pointer is bounded to the allocation in question
   * and so is suitable for direct inclusion into free lists.  Other uses
   * may require amplification.
   */
  struct Remote
  {
    using alloc_id_t = size_t;
    union
    {
      Remote* non_atomic_next;
      std::atomic<Remote*> next{nullptr};
    };

    alloc_id_t allocator_id;

    void set_target_id(alloc_id_t id)
    {
      allocator_id = id;
    }

    alloc_id_t target_id()
    {
      return allocator_id;
    }
  };

  static_assert(
    sizeof(Remote) <= MIN_ALLOC_SIZE,
    "Needs to be able to fit in smallest allocation.");

  struct RemoteAllocator
  {
    using alloc_id_t = Remote::alloc_id_t;
    // Store the message queue on a separate cacheline. It is mutable data that
    // is read by other threads.
    alignas(CACHELINE_SIZE) MPSCQ<Remote> message_queue;

    alloc_id_t id()
    {
      return static_cast<alloc_id_t>(
        reinterpret_cast<uintptr_t>(&message_queue));
    }
  };
} // namespace snmalloc
