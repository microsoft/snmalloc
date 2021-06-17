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

  struct RemoteAllocator
  {
    using alloc_id_t = Remote::alloc_id_t;
    // Store the message queue on a separate cacheline. It is mutable data that
    // is read by other threads.
    alignas(CACHELINE_SIZE)
      MPSCQ<Remote, CapPtrCBAlloc, AtomicCapPtrCBAlloc> message_queue;

    alloc_id_t trunc_id()
    {
      return static_cast<alloc_id_t>(
               reinterpret_cast<uintptr_t>(&message_queue)) &
        ~SIZECLASS_MASK;
    }
  };

  /*
   * A singly-linked list of Remote objects, supporting append and
   * take-all operations.  Intended only for the private use of this
   * allocator; the Remote objects here will later be taken and pushed
   * to the inter-thread message queues.
   */
  struct RemoteList
  {
    /*
     * A stub Remote object that will always be the head of this list;
     * never taken for further processing.
     */
    Remote head{};

    CapPtr<Remote, CBAlloc> last{&head};

    void clear()
    {
      last = CapPtr<Remote, CBAlloc>(&head);
    }

    bool empty()
    {
      return address_cast(last) == address_cast(&head);
    }
  };

  struct RemoteCache
  {
    std::array<RemoteList, REMOTE_SLOTS> list{};

    /// Used to find the index into the array of queues for remote
    /// deallocation
    /// r is used for which round of sending this is.
    template<typename Alloc>
    inline size_t get_slot(size_t id, size_t r)
    {
      constexpr size_t allocator_size = sizeof(Alloc);
      constexpr size_t initial_shift =
        bits::next_pow2_bits_const(allocator_size);
      // static_assert(
      //   initial_shift >= 8,
      //   "Can't embed sizeclass_t into allocator ID low bits");
      SNMALLOC_ASSERT((initial_shift + (r * REMOTE_SLOT_BITS)) < 64);
      return (id >> (initial_shift + (r * REMOTE_SLOT_BITS))) & REMOTE_MASK;
    }

    template<typename SharedStateHandle>
    SNMALLOC_FAST_PATH void dealloc(
      Remote::alloc_id_t target_id,
      CapPtr<void, CBAlloc> p)
    {
      auto r = p.template as_reinterpret<Remote>();

      // TODO fix SharedStateHandle to be size of allocator
      RemoteList* l = &list[get_slot<SharedStateHandle>(target_id, 0)];
      l->last->non_atomic_next = r;
      l->last = r;
    }

    template<typename SharedStateHandle>
    bool post(SharedStateHandle handle, Remote::alloc_id_t id)
    {
      size_t post_round = 0;
      bool sent_something = false;

      while (true)
      {
        // TODO correct size for slot offset
        auto my_slot = get_slot<SharedStateHandle>(id, post_round);

        for (size_t i = 0; i < REMOTE_SLOTS; i++)
        {
          if (i == my_slot)
            continue;

          RemoteList* l = &list[i];
          CapPtr<Remote, CBAlloc> first = l->head.non_atomic_next;

          if (!l->empty())
          {
            MetaEntry entry =
              BackendAllocator::get_meta_data(handle, address_cast(first));
            entry.remote->message_queue.enqueue(first, l->last);
            l->clear();
            sent_something = true;
          }
        }

        RemoteList* resend = &list[my_slot];
        if (resend->empty())
          break;

        // Entries could map back onto the "resend" list,
        // so take copy of the head, mark the last element,
        // and clear the original list.
        CapPtr<Remote, CBAlloc> r = resend->head.non_atomic_next;
        resend->last->non_atomic_next = nullptr;
        resend->clear();

        post_round++;

        while (r != nullptr)
        {
          // Use the next N bits to spread out remote deallocs in our own
          // slot.
          MetaEntry entry =
            BackendAllocator::get_meta_data(handle, address_cast(r));
          auto id = entry.remote->trunc_id();
          // TODO correct size for slot offset
          size_t slot = get_slot<SharedStateHandle>(id, post_round);
          RemoteList* l = &list[slot];
          l->last->non_atomic_next = r;
          l->last = r;

          r = r->non_atomic_next;
        }
      }
      return sent_something;
    }
  };

} // namespace snmalloc
