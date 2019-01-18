#pragma once

#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"
#include "../mem/sizeclass.h"

#include <atomic>

namespace snmalloc
{
  struct Remote
  {
    static const size_t PTR_BITS = sizeof(void*) * 8;
    static const size_t SIZECLASS_BITS =
      bits::next_pow2_bits_const(NUM_SIZECLASSES);
    static const bool USE_TOP_BITS =
      SIZECLASS_BITS + bits::ADDRESS_BITS < PTR_BITS;
    static const uintptr_t SIZECLASS_MASK = ((1ULL << SIZECLASS_BITS) - 1);

    using alloc_id_t = size_t;
    union
    {
      std::atomic<Remote*> next;
      Remote* non_atomic_next;
    };

    // Uses an intptr_t so that when we use the TOP_BITS to encode the
    // sizeclass, then we can use signed shift to correctly handle kernel versus
    // user mode.
    intptr_t value;

    // This will not exist for the minimum object size. This is only used if
    // USE_TOP_BITS is false, and the bottom bit of value is set.
    uint8_t possible_sizeclass;

    void set_sizeclass_and_target_id(alloc_id_t id, uint8_t sizeclass)
    {
      if constexpr (USE_TOP_BITS)
      {
        value = (intptr_t)(
          (id << SIZECLASS_BITS) | ((static_cast<uintptr_t>(sizeclass))));
      }
      else
      {
        assert((id & 1) == 0);
        if (sizeclass == 0)
        {
          value = (intptr_t)(id | 1);
        }
        else
        {
          value = (intptr_t)id;
          possible_sizeclass = sizeclass;
        }
      }
    }

    alloc_id_t target_id()
    {
      if constexpr (USE_TOP_BITS)
      {
        return (alloc_id_t)(value >> SIZECLASS_BITS);
      }
      else
      {
        return (alloc_id_t)(value & ~1);
      }
    }

    uint8_t sizeclass()
    {
      if constexpr (USE_TOP_BITS)
      {
        return (static_cast<uint8_t>((uintptr_t)value & SIZECLASS_MASK));
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
