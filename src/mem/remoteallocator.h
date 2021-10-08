#pragma once

#include "../mem/allocconfig.h"
#include "../mem/freelist.h"
#include "../mem/metaslab.h"
#include "../mem/sizeclasstable.h"

#include <array>
#include <atomic>

namespace snmalloc
{
  // Remotes need to be aligned enough that the bottom bits have enough room for
  // all the size classes, both large and small.
  //
  // Including large classes in this calculation might seem remarkably strange,
  // since large allocations don't have associated Remotes, that is, their
  // remote is taken to be 0.  However, if there are very few small size
  // classes and many large classes, the attempt to align that 0 down by the
  // alignment of a Remote might result in a nonzero value.
  static constexpr size_t REMOTE_MIN_ALIGN = bits::max<size_t>(
    CACHELINE_SIZE,
    bits::max<size_t>(
      bits::next_pow2_const(NUM_SIZECLASSES + 1),
      bits::next_pow2_const(NUM_LARGE_CLASSES + 1)));

  /**
   * Global key for all remote lists.
   */
  inline static FreeListKey key_global(0xdeadbeef, 0xbeefdead, 0xdeadbeef);

  struct alignas(REMOTE_MIN_ALIGN) RemoteAllocator
  {
    using alloc_id_t = address_t;

    // Store the message queue on a separate cacheline. It is mutable data that
    // is read by other threads.
    alignas(CACHELINE_SIZE) FreeObject::AtomicQueuePtr back{nullptr};
    // Store the two ends on different cache lines as access by different
    // threads.
    alignas(CACHELINE_SIZE) FreeObject::QueuePtr front{nullptr};

    constexpr RemoteAllocator() = default;

    void invariant()
    {
      SNMALLOC_ASSERT(back != nullptr);
      SNMALLOC_ASSERT(front != nullptr);
    }

    void init(FreeObject::HeadPtr stub)
    {
      FreeObject::atomic_store_null(stub, key_global);
      front = capptr_rewild(stub);
      back.store(front, std::memory_order_relaxed);
      invariant();
    }

    FreeObject::QueuePtr destroy()
    {
      FreeObject::QueuePtr fnt = front;
      back.store(nullptr, std::memory_order_relaxed);
      front = nullptr;
      return fnt;
    }

    inline bool is_empty()
    {
      FreeObject::QueuePtr bk = back.load(std::memory_order_relaxed);

      return bk == front;
    }

    /**
     * Pushes a list of messages to the queue. Each message from first to
     * last should be linked together through their next pointers.
     */
    template<typename Domesticator>
    void enqueue(
      FreeObject::HeadPtr first,
      FreeObject::HeadPtr last,
      const FreeListKey& key,
      Domesticator domesticate)
    {
      invariant();
      FreeObject::atomic_store_null(last, key);

      // exchange needs to be a release, so nullptr in next is visible.
      FreeObject::QueuePtr prev =
        back.exchange(capptr_rewild(last), std::memory_order_release);

      FreeObject::atomic_store_next(domesticate(prev), first, key);
    }

    FreeObject::QueuePtr peek()
    {
      return front;
    }

    /**
     * Returns the front message, or null if not possible to return a message.
     */
    template<typename Domesticator>
    std::pair<FreeObject::HeadPtr, bool>
    dequeue(const FreeListKey& key, Domesticator domesticate)
    {
      invariant();
      FreeObject::HeadPtr first = domesticate(front);
      FreeObject::HeadPtr next = first->atomic_read_next(key, domesticate);

      if (next != nullptr)
      {
        /*
         * We've domesticate_queue-d next so that we can read through it, but
         * we're storing it back into client-accessible memory in
         * !QueueHeadsAreTame builds, so go ahead and consider it Wild again.
         * On QueueHeadsAreTame builds, the subsequent domesticate_head call
         * above will also be a type-level sleight of hand, but we can still
         * justify it by the domesticate_queue that happened in this dequeue().
         */
        front = capptr_rewild(next);
        invariant();
        return {first, true};
      }

      return {nullptr, false};
    }

    alloc_id_t trunc_id()
    {
      return address_cast(this);
    }
  };
} // namespace snmalloc
