#pragma once

#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"

#include <atomic>

namespace snmalloc
{
  struct Remote
  {
    static const uint64_t SIZECLASS_SHIFT = 56;
    static const uint64_t SIZECLASS_MASK = 0xffULL << SIZECLASS_SHIFT;
    static const uint64_t TARGET_MASK = ~SIZECLASS_MASK;

    static_assert(SIZECLASS_MASK == 0xff00'0000'0000'0000ULL);

    using alloc_id_t = size_t;
    union
    {
      std::atomic<Remote*> next;
      Remote* non_atomic_next;
    };

    uint64_t value;

    void set_target_id(alloc_id_t id)
    {
      assert(id == (id & TARGET_MASK));
      value = (id & TARGET_MASK) | (value & SIZECLASS_MASK);
    }

    void set_sizeclass(uint8_t sizeclass)
    {
      value = (value & TARGET_MASK) |
        ((static_cast<uint64_t>(sizeclass) << SIZECLASS_SHIFT) &
         SIZECLASS_MASK);
    }

    alloc_id_t target_id()
    {
      return value & TARGET_MASK;
    }

    uint8_t sizeclass()
    {
      return (value & SIZECLASS_MASK) >> SIZECLASS_SHIFT;
    }
  };

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
}
