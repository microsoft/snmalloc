#pragma once

#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"
#include "../mem/sizeclass.h"

#include <atomic>

namespace snmalloc
{
  struct Remote
  {
    using alloc_id_t = size_t;
    union
    {
      std::atomic<Remote*> next;
      Remote* non_atomic_next;
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
