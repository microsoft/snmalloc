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
  template<typename Config, size_t RINGS>
  class RemoteDeallocCacheBatching
  {
    static_assert(RINGS > 0);

    std::array<freelist::Builder<false, true>, RINGS> open_builder;
    std::array<typename Config::PagemapEntry::SlabMetadata*, RINGS> open_meta =
      {nullptr};

    SNMALLOC_FAST_PATH size_t
    ring_set(typename Config::PagemapEntry::SlabMetadata* meta)
    {
      // See https://github.com/skeeto/hash-prospector for choice of constant
      return ((meta->as_key_tweak() * 0x7EFB352D) >> 16) &
        bits::mask_bits(DEALLOC_BATCH_RING_SET_BITS);
    }

    template<typename Forward>
    SNMALLOC_FAST_PATH void close_one_pending(Forward forward, size_t ix)
    {
      auto rmsg = BatchedRemoteMessage::mk_from_freelist_builder(
        open_builder[ix],
        freelist::Object::key_root,
        open_meta[ix]->as_key_tweak());

      auto& entry = Config::Backend::get_metaentry(address_cast(rmsg));

      forward(entry.get_remote()->trunc_id(), rmsg);

      open_meta[ix] = nullptr;
    }

    SNMALLOC_FAST_PATH void init_one_pending(
      size_t ix, typename Config::PagemapEntry::SlabMetadata* meta)
    {
      open_builder[ix].init(
        0, freelist::Object::key_root, open_meta[ix]->as_key_tweak());
      open_meta[ix] = meta;
    }

  public:
    template<typename Forward>
    SNMALLOC_FAST_PATH void dealloc(
      typename Config::PagemapEntry::SlabMetadata* meta,
      freelist::HeadPtr r,
      Forward forward)
    {
      size_t ix_set = ring_set(meta);

      for (size_t ix_way = 0; ix_way < DEALLOC_BATCH_RING_ASSOC; ix_way++)
      {
        size_t ix = ix_set + ix_way;
        if (meta == open_meta[ix])
        {
          open_builder[ix].add(
            r, freelist::Object::key_root, meta->as_key_tweak());
          return;
        }
      }

      size_t victim_ix = ix_set;
      size_t victim_size = 0;
      for (size_t ix_way = 0; ix_way < DEALLOC_BATCH_RING_ASSOC; ix_way++)
      {
        size_t ix = ix_set + ix_way;
        if (open_meta[ix] == nullptr)
        {
          victim_ix = ix;
          break;
        }

        size_t szix = open_builder[ix].extract_segment_length();
        if (szix > victim_size)
        {
          victim_size = szix;
          victim_ix = ix;
        }
      }

      if (open_meta[victim_ix] != nullptr)
      {
        close_one_pending(forward, victim_ix);
      }
      init_one_pending(victim_ix, meta);

      open_builder[victim_ix].add(
        r, freelist::Object::key_root, meta->as_key_tweak());
    }

    template<typename Forward>
    SNMALLOC_FAST_PATH void close_all(Forward forward)
    {
      for (size_t ix = 0; ix < RINGS; ix++)
      {
        if (open_meta[ix] != nullptr)
        {
          close_one_pending(forward, ix);
          open_meta[ix] = nullptr;
        }
      }
    }

    void init()
    {
      open_meta = {nullptr};
    }
  };

  template<typename Config>
  struct RemoteDeallocCacheNoBatching
  {
    void init() {}

    template<typename Forward>
    void close_all(Forward)
    {}

    template<typename Forward>
    SNMALLOC_FAST_PATH void dealloc(
      typename Config::PagemapEntry::SlabMetadata*,
      freelist::HeadPtr r,
      Forward forward)
    {
      auto& entry = Config::Backend::get_metaentry(address_cast(r));
      forward(
        entry.get_remote()->trunc_id(),
        SingletonRemoteMessage::emplace_in_alloc(r.as_void()));
    }
  };

  template<typename Config>
  using RemoteDeallocCacheBatchingImpl = std::conditional_t<
    (DEALLOC_BATCH_RINGS > 0),
    RemoteDeallocCacheBatching<Config, DEALLOC_BATCH_RINGS>,
    RemoteDeallocCacheNoBatching<Config>>;

  /**
   * Stores the remote deallocation to batch them before sending
   */
  template<typename Config>
  struct RemoteDeallocCache
  {
    std::array<freelist::Builder<false>, REMOTE_SLOTS> list;

    RemoteDeallocCacheBatchingImpl<Config> batching;

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
    SNMALLOC_FAST_PATH bool reserve_space(const Entry& entry, uint16_t n = 1)
    {
      static_assert(sizeof(n) * 8 > MAX_CAPACITY_BITS);

      auto size =
        n * static_cast<int64_t>(sizeclass_full_to_size(entry.get_sizeclass()));

      bool result = capacity > size;
      if (result)
        capacity -= size;
      return result;
    }

    template<size_t allocator_size>
    SNMALLOC_FAST_PATH void forward(
      RemoteAllocator::alloc_id_t target_id, capptr::Alloc<RemoteMessage> msg)
    {
      list[get_slot<allocator_size>(target_id, 0)].add(
        RemoteMessage::to_message_link(msg),
        RemoteAllocator::key_global,
        NO_KEY_TWEAK);
    }

    template<size_t allocator_size>
    SNMALLOC_FAST_PATH void dealloc(
      typename Config::PagemapEntry::SlabMetadata* meta, capptr::Alloc<void> p)
    {
      SNMALLOC_ASSERT(initialised);

      auto r = freelist::Object::make<capptr::bounds::AllocWild>(p);

      batching.dealloc(
        meta,
        r,
        [this](
          RemoteAllocator::alloc_id_t target_id,
          capptr::Alloc<RemoteMessage> msg) {
          forward<allocator_size>(target_id, msg);
        });
    }

    template<size_t allocator_size>
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

      batching.close_all([this](
                           RemoteAllocator::alloc_id_t target_id,
                           capptr::Alloc<RemoteMessage> msg) {
        forward<allocator_size>(target_id, msg);
      });

      while (true)
      {
        auto my_slot = get_slot<allocator_size>(id, post_round);

        for (size_t i = 0; i < REMOTE_SLOTS; i++)
        {
          if (i == my_slot)
            continue;

          if (!list[i].empty())
          {
            auto [first_, last_] = list[i].extract_segment(key, NO_KEY_TWEAK);
            auto first = RemoteMessage::from_message_link(first_);
            auto last = RemoteMessage::from_message_link(last_);
            const auto& entry =
              Config::Backend::get_metaentry(address_cast(first_));
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
        list[my_slot].close(resend, key, NO_KEY_TWEAK);

        post_round++;

        while (!resend.empty())
        {
          // Use the next N bits to spread out remote deallocs in our own
          // slot.
          auto r = resend.take(key, domesticate);
          const auto& entry = Config::Backend::get_metaentry(address_cast(r));
          auto i = entry.get_remote()->trunc_id();
          size_t slot = get_slot<allocator_size>(i, post_round);
          list[slot].add(r, key, NO_KEY_TWEAK);
        }
      }

      // Reset capacity as we have emptied everything
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
        l.init(0, RemoteAllocator::key_global, NO_KEY_TWEAK);
      }
      capacity = REMOTE_CACHE;

      batching.init();
    }
  };
} // namespace snmalloc
