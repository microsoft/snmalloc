#pragma once

#include "../ds/mpscq.h"
#include "../mem/allocconfig.h"
#include "../mem/freelist.h"
#include "../mem/metaslab.h"
#include "../mem/remoteallocator.h"
#include "../mem/sizeclasstable.h"

#include <array>
#include <atomic>

namespace snmalloc
{
  /**
   * Stores the remote deallocation to batch them before sending
   */
  struct RemoteDeallocCache
  {
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

      /**
       * Initially is null ptr, and needs to be non-null before anything runs on
       * this.
       */
      CapPtr<Remote, CBAlloc> last{nullptr};

      void clear()
      {
        last = CapPtr<Remote, CBAlloc>(&head);
      }

      bool empty()
      {
        return address_cast(last) == address_cast(&head);
      }

      constexpr RemoteList() {}
    };

    std::array<RemoteList, REMOTE_SLOTS> list{};

    /**
     * The total amount of memory we are waiting for before we will dispatch
     * to other allocators. Zero can mean we have not initialised the allocator
     * yet. This is initialised to the 0 so that we always hit a slow path to
     * start with, when we hit the slow path and need to dispatch everything, we
     * can check if we are a real allocator and lazily provide a real allocator.
     */
    int64_t capacity{0};

#ifndef NDEBUG
    bool initialised = false;
#endif

    /// Used to find the index into the array of queues for remote
    /// deallocation
    /// r is used for which round of sending this is.
    template<size_t allocator_size>
    inline size_t get_slot(size_t id, size_t r)
    {
      constexpr size_t initial_shift =
        bits::next_pow2_bits_const(allocator_size);
      // static_assert(
      //   initial_shift >= 8,
      //   "Can't embed sizeclass_t into allocator ID low bits");
      SNMALLOC_ASSERT((initial_shift + (r * REMOTE_SLOT_BITS)) < 64);
      return (id >> (initial_shift + (r * REMOTE_SLOT_BITS))) & REMOTE_MASK;
    }

    /**
     * Checks if the capacity has enough to cache an entry from this
     * slab. Returns true, if this does not overflow the budget.
     *
     * This does not require initialisation to be safely called.
     */
    SNMALLOC_FAST_PATH bool reserve_space(const MetaEntry& entry)
    {
      auto size =
        static_cast<int64_t>(sizeclass_to_size(entry.get_sizeclass()));

      bool result = capacity > size;
      if (result)
        capacity -= size;
      return result;
    }

    template<size_t allocator_size>
    SNMALLOC_FAST_PATH void
    dealloc(Remote::alloc_id_t target_id, CapPtr<void, CBAlloc> p)
    {
      SNMALLOC_ASSERT(initialised);
      auto r = p.template as_reinterpret<Remote>();

      RemoteList* l = &list[get_slot<allocator_size>(target_id, 0)];
      l->last->non_atomic_next = r;
      l->last = r;
    }

    template<size_t allocator_size, typename SharedStateHandle>
    bool post(SharedStateHandle handle, Remote::alloc_id_t id)
    {
      SNMALLOC_ASSERT(initialised);
      size_t post_round = 0;
      bool sent_something = false;

      while (true)
      {
        auto my_slot = get_slot<allocator_size>(id, post_round);

        for (size_t i = 0; i < REMOTE_SLOTS; i++)
        {
          if (i == my_slot)
            continue;

          RemoteList* l = &list[i];
          CapPtr<Remote, CBAlloc> first = l->head.non_atomic_next;

          if (!l->empty())
          {
            MetaEntry entry = SharedStateHandle::Backend::get_meta_data(
              handle.get_backend_state(), address_cast(first));
            entry.get_remote()->message_queue.enqueue(first, l->last);
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
          MetaEntry entry = SharedStateHandle::Backend::get_meta_data(
            handle.get_backend_state(), address_cast(r));
          auto id = entry.get_remote()->trunc_id();
          // TODO correct size for slot offset
          size_t slot = get_slot<allocator_size>(id, post_round);
          RemoteList* l = &list[slot];
          l->last->non_atomic_next = r;
          l->last = r;

          r = r->non_atomic_next;
        }
      }

      // Reset capacity as we have empty everything
      capacity = REMOTE_CACHE;

      return sent_something;
    }

    /**
     * Constructor design to allow constant init
     */
    constexpr RemoteDeallocCache() {}

    /**
     * Must be called before anything else to ensure actually initialised
     * not just zero init.
     */
    void init()
    {
#ifndef NDEBUG
      initialised = true;
#endif
      for (auto& l : list)
      {
        SNMALLOC_ASSERT(l.last == nullptr || l.empty());
        l.clear();
      }
      capacity = REMOTE_CACHE;
    }
  };
} // namespace snmalloc
