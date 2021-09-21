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
    alignas(CACHELINE_SIZE)
      FreeObject::AtomicQueuePtr<capptr::bounds::Alloc> back{nullptr};
    // Store the two ends on different cache lines as access by different
    // threads.
    alignas(CACHELINE_SIZE) FreeObject::QueuePtr<capptr::bounds::Alloc> front{
      nullptr};

    constexpr RemoteAllocator() = default;

    void invariant()
    {
      SNMALLOC_ASSERT(back != nullptr);
      SNMALLOC_ASSERT(front != nullptr);
    }

    void init(FreeObject::QueuePtr<capptr::bounds::Alloc> stub)
    {
      FreeObject::atomic_store_null(stub, key_global);
      front = stub;
      back.store(stub, std::memory_order_relaxed);
      invariant();
    }

    FreeObject::QueuePtr<capptr::bounds::Alloc> destroy()
    {
      FreeObject::QueuePtr<capptr::bounds::Alloc> fnt = front;
      back.store(nullptr, std::memory_order_relaxed);
      front = nullptr;
      return fnt;
    }

    inline bool is_empty()
    {
      FreeObject::QueuePtr<capptr::bounds::Alloc> bk =
        back.load(std::memory_order_relaxed);

      return bk == front;
    }

    /**
     * Pushes a list of messages to the queue. Each message from first to
     * last should be linked together through their next pointers.
     */
    void enqueue(
      FreeObject::QueuePtr<capptr::bounds::Alloc> first,
      FreeObject::QueuePtr<capptr::bounds::Alloc> last,
      const FreeListKey& key)
    {
      invariant();
      FreeObject::atomic_store_null(last, key);

      // exchange needs to be a release, so nullptr in next is visible.
      FreeObject::QueuePtr<capptr::bounds::Alloc> prev =
        back.exchange(last, std::memory_order_release);

      // XXX prev is not known to be domesticated
      FreeObject::atomic_store_next(prev, first, key);
    }

    FreeObject::QueuePtr<capptr::bounds::Alloc> peek()
    {
      return front;
    }

    /**
     * Returns the front message, or null if not possible to return a message.
     */
    std::pair<
      FreeObject::HeadPtr<capptr::bounds::Alloc, capptr::bounds::Alloc>,
      bool>
    dequeue(const FreeListKey& key)
    {
      auto domesticate = [](FreeObject::QueuePtr<capptr::bounds::Alloc> p)
                           SNMALLOC_FAST_PATH_LAMBDA { return p; };
      invariant();
      FreeObject::HeadPtr<capptr::bounds::Alloc, capptr::bounds::Alloc> first =
        domesticate(front);
      FreeObject::QueuePtr<capptr::bounds::Alloc> next =
        first->atomic_read_next(key, domesticate);

      if (next != nullptr)
      {
        front = next;
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
