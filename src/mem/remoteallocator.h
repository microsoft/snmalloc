#pragma once

#include "../backend/backend.h"
#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"
#include "../mem/freelist.h"
#include "../mem/metaslab.h"
#include "../mem/sizeclass.h"

#include <array>
#include <atomic>

#ifdef CHECK_CLIENT
#  define SNMALLOC_DONT_CACHE_ALLOCATOR_PTR
#endif

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
      CapPtr<Remote, CBAlloc> non_atomic_next;
      AtomicCapPtr<Remote, CBAlloc> next{nullptr};
    };

    constexpr Remote() {}

    /** Zero out a Remote tracking structure, return pointer to object base */
    template<capptr_bounds B>
    SNMALLOC_FAST_PATH static CapPtr<void, B> clear(CapPtr<Remote, B> self)
    {
      pal_zero<Pal>(self, sizeof(Remote));
      return self.as_void();
    }
  };

  static_assert(
    sizeof(Remote) <= MIN_ALLOC_SIZE,
    "Needs to be able to fit in smallest allocation.");

  // Remotes need to be aligned enough that all the
  // small size classes can fit in the bottom bits.
  static constexpr size_t REMOTE_MIN_ALIGN = bits::min<size_t>(
    CACHELINE_SIZE, bits::next_pow2_const(NUM_SIZECLASSES + 1));

  struct alignas(REMOTE_MIN_ALIGN) RemoteAllocator
  {
    using alloc_id_t = Remote::alloc_id_t;
    // Store the message queue on a separate cacheline. It is mutable data that
    // is read by other threads.

    MPSCQ<Remote, CapPtrCBAlloc, AtomicCapPtrCBAlloc> message_queue;

    alloc_id_t trunc_id()
    {
      return static_cast<alloc_id_t>(
               reinterpret_cast<uintptr_t>(&message_queue)) &
        ~SIZECLASS_MASK;
    }
  };
} // namespace snmalloc
