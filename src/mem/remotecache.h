#pragma once

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
    std::array<FreeListBuilder<false, false>, REMOTE_SLOTS> list;

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
    inline size_t get_slot(size_t i, size_t r)
    {
      constexpr size_t initial_shift =
        bits::next_pow2_bits_const(allocator_size);
      // static_assert(
      //   initial_shift >= 8,
      //   "Can't embed sizeclass_t into allocator ID low bits");
      SNMALLOC_ASSERT((initial_shift + (r * REMOTE_SLOT_BITS)) < 64);
      return (i >> (initial_shift + (r * REMOTE_SLOT_BITS))) & REMOTE_MASK;
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
    SNMALLOC_FAST_PATH void dealloc(
      RemoteAllocator::alloc_id_t target_id,
      CapPtr<void, CBAlloc> p,
      const FreeListKey& key)
    {
      SNMALLOC_ASSERT(initialised);
      auto r = p.template as_reinterpret<FreeObject>();

      list[get_slot<allocator_size>(target_id, 0)].add(r, key);
    }

    template<size_t allocator_size, typename SharedStateHandle>
    bool post(
      SharedStateHandle handle,
      RemoteAllocator::alloc_id_t id,
      const FreeListKey& key)
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

          if (!list[i].empty())
          {
            auto [first, last] = list[i].extract_segment(key);
            MetaEntry entry = SharedStateHandle::Backend::get_meta_data(
              handle.get_backend_state(), address_cast(first));
            entry.get_remote()->enqueue(first, last, key);
            sent_something = true;
          }
        }

        if (list[my_slot].empty())
          break;

        // Entries could map back onto the "resend" list,
        // so take copy of the head, mark the last element,
        // and clear the original list.
        FreeListIter resend;
        list[my_slot].close(resend, key);

        post_round++;

        while (!resend.empty())
        {
          // Use the next N bits to spread out remote deallocs in our own
          // slot.
          auto r = resend.take(key);
          MetaEntry entry = SharedStateHandle::Backend::get_meta_data(
            handle.get_backend_state(), address_cast(r));
          auto i = entry.get_remote()->trunc_id();
          size_t slot = get_slot<allocator_size>(i, post_round);
          list[slot].add(r, key);
        }
      }

      // Reset capacity as we have empty everything
      capacity = REMOTE_CACHE;

      return sent_something;
    }

    /**
     * Constructor design to allow constant init
     */
    constexpr RemoteDeallocCache() = default;

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
        l.init();
      }
      capacity = REMOTE_CACHE;
    }
  };
} // namespace snmalloc
