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
   */
  struct Remote
  {
    using alloc_id_t = size_t;
    union
    {
      Remote* non_atomic_next;
      std::atomic<Remote*> next{nullptr};
    };

    /*
     * We embed the size class in the bottom 8 bits of an allocator ID (i.e.,
     * the address of an Alloc's remote_alloc's message_queue; in practice we
     * only need 7 bits, but using 8 is conjectured to be faster).  The hashing
     * algorithm of the Alloc's RemoteCache already ignores the bottom
     * "initial_shift" bits, which is, in practice, well above 8.  There's a
     * static_assert() over there that helps ensure this stays true.
     *
     * This does mean that we might have message_queues that always collide in
     * the hash algorithm, if they're within "initial_shift" of each other. Such
     * pairings will substantially decrease performance and so we prohibit them
     * and use SNMALLOC_ASSERT to verify that they do not exist in debug builds.
     */
    alloc_id_t alloc_id_and_sizeclass;

    void set_info(alloc_id_t id, sizeclass_t sc)
    {
      alloc_id_and_sizeclass = (id & ~SIZECLASS_MASK) | sc;
    }

    alloc_id_t trunc_target_id()
    {
      return alloc_id_and_sizeclass & ~SIZECLASS_MASK;
    }

    sizeclass_t sizeclass()
    {
      return alloc_id_and_sizeclass & SIZECLASS_MASK;
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

    alloc_id_t trunc_id()
    {
      return static_cast<alloc_id_t>(
               reinterpret_cast<uintptr_t>(&message_queue)) &
        ~SIZECLASS_MASK;
    }
  };
} // namespace snmalloc
