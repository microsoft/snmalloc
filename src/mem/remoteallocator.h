#pragma once

#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"

#include <atomic>

namespace snmalloc
{
  struct Remote
  {
    static const size_t PTR_BITS = sizeof(void*) * 8;
    static const size_t SIZECLASS_BITS = sizeof(uint8_t) * 8;
    static const bool USE_TOP_BITS =
      SIZECLASS_BITS + bits::ADDRESS_BITS <= PTR_BITS;
    static const uintptr_t SIZECLASS_SHIFT = PTR_BITS - SIZECLASS_BITS;
    static const uintptr_t SIZECLASS_MASK = ((1ULL << SIZECLASS_BITS) - 1)
      << SIZECLASS_SHIFT;
    static const uintptr_t TARGET_MASK = ~SIZECLASS_MASK;

    using alloc_id_t = size_t;
    union
    {
      std::atomic<Remote*> next;
      Remote* non_atomic_next;
    };

    uintptr_t value;
    // This will not exist for the minimum object size. This is only used if
    // USE_TOP_BITS is false, and the bottom bit of value is set.
    uint8_t possible_sizeclass;

    void set_sizeclass_and_target_id(alloc_id_t id, uint8_t sizeclass)
    {
      if constexpr (USE_TOP_BITS)
      {
        assert(id == (id & TARGET_MASK));
        value = (id & TARGET_MASK) |
          ((static_cast<uint64_t>(sizeclass) << SIZECLASS_SHIFT) &
           SIZECLASS_MASK);
      }
      else
      {
        assert((id & 1) == 0);
        if (sizeclass == 0)
        {
          value = id | 1;
        }
        else
        {
          value = id;
          possible_sizeclass = sizeclass;
        }
      }
    }

    alloc_id_t target_id()
    {
      if constexpr (USE_TOP_BITS)
      {
        return value & TARGET_MASK;
      }
      else
      {
        return value & ~1;
      }
    }

    uint8_t sizeclass()
    {
      if constexpr (USE_TOP_BITS)
      {
        return (value & SIZECLASS_MASK) >> SIZECLASS_SHIFT;
      }
      else
      {
        return ((value & 1) == 1) ? 0 : possible_sizeclass;
      }
    }
  };

  static_assert(
    (offsetof(Remote, possible_sizeclass)) <= MIN_ALLOC_SIZE,
    "Need to be able to cast any small alloc to Remote");

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
