// SPDX-License-Identifier: MIT
//
// Heap profiler -- global lock-free intrusive list of currently-sampled
// allocations.
//
// Phase 2.2 of the heap-profiling milestone. Purely additive.
//
// Design (chosen Design A from research, see synthesis):
//   - Singly-linked intrusive Treiber stack on `head_`.
//   - Tombstone bit packed into the low bit of `SampledAlloc::next`
//     (which is the same word read by traversers, so liveness + link
//     come from a single atomic load -- no torn read).
//   - Removal is two phases:
//       (1) CAS the tombstone bit on `node->next` (linearisation point).
//       (2) Best-effort physical unlink via a linear scan.
//     If (2) loses a race, the node lingers as a tombstoned skip in the
//     list; the next snapshot or remove pass reaps it. No reclamation
//     ordering needed because node memory is owned by the NodePool, not
//     by the list.
//   - Push appends at head with a release CAS.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"

#include <atomic>
#include <cstdint>

namespace snmalloc::profile
{
  /**
   * Lock-free intrusive list of SampledAlloc nodes.
   *
   * Invariants:
   *   - A node is on the list iff at some point a push() linked it AND
   *     no successful tombstone CAS has since fired on its `next` field.
   *   - `next` low bit = tombstone marker. SampledAlloc is cache-line
   *     aligned, so the low bit of any node pointer is always free.
   *   - Readers tolerate concurrent push/remove. push() may or may not
   *     be visible to an in-flight snapshot; remove() (tombstone CAS) is
   *     visible to any snapshot that acquire-loads `next` after it.
   */
  class SampledList
  {
  public:
    static constexpr uintptr_t kTombstoneBit = 1;

    [[nodiscard]] static SampledAlloc* untag(uintptr_t p) noexcept
    {
      return reinterpret_cast<SampledAlloc*>(p & ~kTombstoneBit);
    }

    [[nodiscard]] static bool is_tombstoned(uintptr_t p) noexcept
    {
      return (p & kTombstoneBit) != 0;
    }

    [[nodiscard]] static uintptr_t tag(SampledAlloc* p, bool tomb) noexcept
    {
      return reinterpret_cast<uintptr_t>(p) | (tomb ? kTombstoneBit : 0);
    }

    SampledList() noexcept = default;
    SampledList(const SampledList&) = delete;
    SampledList& operator=(const SampledList&) = delete;

    /**
     * Publish a freshly-acquired node on the list.
     *
     * Wait-free in the absence of contention; lock-free under contention.
     * On return, any snapshot that acquire-loads `head_` after this call
     * sees `node` with its fully-initialised payload (release CAS).
     */
    void push(SampledAlloc* node) noexcept
    {
      SampledAlloc* old_head = head_.load(std::memory_order_relaxed);
      for (;;)
      {
        node->next.store(tag(old_head, false), std::memory_order_relaxed);
        if (head_.compare_exchange_weak(
              old_head,
              node,
              std::memory_order_release,
              std::memory_order_relaxed))
        {
          return;
        }
      }
    }

    /**
     * Mark a node as removed. Lock-free. Safe to call from any thread,
     * including one that did not push the node (cross-thread dealloc).
     *
     * Returns true if this call performed the tombstone transition,
     * false if the node was already tombstoned by someone else.
     */
    bool remove(SampledAlloc* node) noexcept
    {
      if (node == nullptr)
        return false;

      // Step 1: tombstone CAS -- linearisation point.
      uintptr_t cur = node->next.load(std::memory_order_relaxed);
      for (;;)
      {
        if (is_tombstoned(cur))
          return false;
        if (node->next.compare_exchange_weak(
              cur,
              cur | kTombstoneBit,
              std::memory_order_release,
              std::memory_order_relaxed))
          break;
      }

      // Step 2: best-effort physical unlink. Failure is fine; tombstoned
      // nodes are skipped by the snapshot reader.
      try_unlink(node);
      return true;
    }

    /**
     * Walk the list and invoke `fn(node)` for every non-tombstoned node.
     * Returns the count of live nodes visited.
     *
     * Tolerates concurrent push (may or may not see the new node) and
     * concurrent remove (skips tombstoned). The reader must NOT call
     * remove() during the walk -- snapshots are read-only.
     */
    template<typename F>
    size_t snapshot(F&& fn) const noexcept
    {
      size_t live = 0;
      SampledAlloc* cur = head_.load(std::memory_order_acquire);
      while (cur != nullptr)
      {
        uintptr_t n = cur->next.load(std::memory_order_acquire);
        if (!is_tombstoned(n))
        {
          fn(cur);
          ++live;
        }
        cur = untag(n);
      }
      return live;
    }

    /// Snapshot helper that just counts live nodes. Used by tests.
    [[nodiscard]] size_t debug_count() const noexcept
    {
      return snapshot([](SampledAlloc*) {});
    }

    /// Test-only: empty the list of all (live + tombstoned) nodes, returning
    /// each one to the caller via `fn(node)` so the caller can return it to
    /// the node pool. Not safe to call concurrently with push/remove/snapshot.
    template<typename F>
    void debug_drain(F&& fn) noexcept
    {
      SampledAlloc* cur = head_.exchange(nullptr, std::memory_order_acq_rel);
      while (cur != nullptr)
      {
        SampledAlloc* next = untag(cur->next.load(std::memory_order_relaxed));
        cur->next.store(0, std::memory_order_relaxed);
        fn(cur);
        cur = next;
      }
    }

  private:
    /**
     * Walk the list searching for `node`; CAS predecessor's next past it.
     * Best-effort: on a lost race the node remains tombstoned and the next
     * walk will reap it.
     */
    void try_unlink(SampledAlloc* node) noexcept
    {
      uintptr_t node_next = node->next.load(std::memory_order_acquire);
      // `node_next` carries node's tombstone bit; the successor pointer
      // is whatever next field pointed at when we tombstoned it.
      SampledAlloc* succ = untag(node_next);

      // Special-case: node at head.
      SampledAlloc* h = head_.load(std::memory_order_acquire);
      if (h == node)
      {
        if (head_.compare_exchange_strong(
              h,
              succ,
              std::memory_order_release,
              std::memory_order_relaxed))
          return;
        // Lost race -- fall through to scan.
      }

      // Linear search from current head.
      SampledAlloc* prev = head_.load(std::memory_order_acquire);
      while (prev != nullptr)
      {
        if (prev == node)
          return; // node still at head; another snapshot/remove may handle.
        uintptr_t pn = prev->next.load(std::memory_order_acquire);
        if (is_tombstoned(pn))
        {
          // Skip tombstoned predecessor; its eventual unlink will splice
          // anything attached to it.
          prev = untag(pn);
          continue;
        }
        SampledAlloc* nxt = untag(pn);
        if (nxt == node)
        {
          // CAS prev->next from "points to node, not tombstoned"
          // to "points to succ, not tombstoned". The desired value is
          // tag(succ, false) regardless of node's tombstone bit
          // (the tombstone bit on prev->next belongs to prev, not node).
          uintptr_t expected = tag(node, false);
          uintptr_t desired = tag(succ, false);
          prev->next.compare_exchange_strong(
            expected,
            desired,
            std::memory_order_release,
            std::memory_order_relaxed);
          return;
        }
        prev = nxt;
      }
    }

    alignas(kCacheLineSize) std::atomic<SampledAlloc*> head_{nullptr};
  };
} // namespace snmalloc::profile
