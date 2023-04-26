#pragma once

#include "../ds/ds.h"
#include "backend_wrappers.h"
#include "freelist.h"
#include "metadata.h"
#include "remoteallocator.h"
#include "sizeclasstable.h"

#include <array>
#include <atomic>

namespace snmalloc
{
  /**
   * Stores the remote deallocation to batch them before sending
   */
  struct RemoteDeallocCache
  {
    std::array<freelist::Builder<false>, REMOTE_SLOTS> list;

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
    template<typename Entry>
    SNMALLOC_FAST_PATH bool reserve_space(const Entry& entry)
    {
      auto size =
        static_cast<int64_t>(sizeclass_full_to_size(entry.get_sizeclass()));

      bool result = capacity > size;
      if (result)
        capacity -= size;
      return result;
    }

    template<size_t allocator_size>
    SNMALLOC_FAST_PATH void
    dealloc(RemoteAllocator::alloc_id_t target_id, capptr::Alloc<void> p)
    {
      SNMALLOC_ASSERT(initialised);
      auto r = p.template as_reinterpret<freelist::Object::T<>>();

      list[get_slot<allocator_size>(target_id, 0)].add(
        r, RemoteAllocator::key_global);
    }

    template<size_t allocator_size, typename Config>
    bool post(
      typename Config::LocalState* local_state, RemoteAllocator::alloc_id_t id)
    {
      // Use same key as the remote allocator, so segments can be
      // posted to a remote allocator without reencoding.
      const auto& key = RemoteAllocator::key_global;
      SNMALLOC_ASSERT(initialised);
      size_t post_round = 0;
      bool sent_something = false;
      auto domesticate = [local_state](freelist::QueuePtr p)
                           SNMALLOC_FAST_PATH_LAMBDA {
                             return capptr_domesticate<Config>(local_state, p);
                           };

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
            const auto& entry =
              Config::Backend::get_metaentry(address_cast(first));
            auto remote = entry.get_remote();
            // If the allocator is not correctly aligned, then the bit that is
            // set implies this is used by the backend, and we should not be
            // deallocating memory here.
            snmalloc_check_client(
              mitigations(sanity_checks),
              !entry.is_backend_owned(),
              "Delayed detection of attempt to free internal structure.");
            if constexpr (Config::Options.QueueHeadsAreTame)
            {
              auto domesticate_nop = [](freelist::QueuePtr p) {
                return freelist::HeadPtr::unsafe_from(p.unsafe_ptr());
              };
              remote->enqueue(first, last, domesticate_nop);
            }
            else
            {
              remote->enqueue(first, last, domesticate);
            }
            sent_something = true;
          }
        }

        if (list[my_slot].empty())
          break;

        // Entries could map back onto the "resend" list,
        // so take copy of the head, mark the last element,
        // and clear the original list.
        freelist::Iter<> resend;
        list[my_slot].close(resend, key);

        post_round++;

        while (!resend.empty())
        {
          // Use the next N bits to spread out remote deallocs in our own
          // slot.
          auto r = resend.take(key, domesticate);
          const auto& entry = Config::Backend::get_metaentry(address_cast(r));
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
        // We do not need to initialise with a particular slab, so pass
        // a null address.
        l.init(0, RemoteAllocator::key_global);
      }
      capacity = REMOTE_CACHE;
    }
  };
} // namespace snmalloc
