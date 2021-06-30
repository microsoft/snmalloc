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

#ifdef SNMALLOC_DONT_CACHE_ALLOCATOR_PTR
    /**
     * Cache the size class of the object to improve performance.
     *
     * This implementation does not cache the allocator id due to security
     * concerns. Alternative implementations may store the allocator
     * id, so that amplification costs can be mitigated on CHERI with MTE.
     */
    sizeclass_t sizeclasscache;
#else
    /* This implementation assumes that storing the allocator ID in a freed
     * object is not a security concern.  Either we trust the code running on
     * top of the allocator, or additional security measure are in place such
     * as MTE + CHERI.
     *
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
#endif

    /**
     * Set up a remote object.  Potentially cache sizeclass and allocator id.
     */
    void set_info(alloc_id_t id, sizeclass_t sc)
    {
#ifdef SNMALLOC_DONT_CACHE_ALLOCATOR_PTR
      UNUSED(id);
      sizeclasscache = sc;
#else
      alloc_id_and_sizeclass = (id & ~SIZECLASS_MASK) | sc;
#endif
    }

    sizeclass_t sizeclass()
    {
#ifdef SNMALLOC_DONT_CACHE_ALLOCATOR_PTR
      return sizeclasscache;
#else
      return alloc_id_and_sizeclass & SIZECLASS_MASK;
#endif
    }

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
