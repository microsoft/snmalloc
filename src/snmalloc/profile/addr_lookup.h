// SPDX-License-Identifier: MIT
//
// Heap profiler -- address -> alloc-site reverse lookup (Phase 10.1B).
//
// Given an arbitrary heap address (e.g. a sample from a PMU-driven sampler
// such as Linux perf cycle/cache-miss events), return the captured
// alloc-time call stack for the originating sampled allocation -- if and
// only if that allocation is still live AND was itself selected by the
// Poisson sampler.
//
// Design choice (per the Phase 10.1 scope guardrails): rather than thread
// an interval tree into the lock-free SampledList, this header builds a
// transient sorted index from a single SampledList snapshot at lookup
// time.  Costs:
//
//   - O(N log N) build per call (sort by base address).
//   - O(log N) binary-search query.
//
// where N is the count of currently-live sampled allocations.  With the
// default 512 KiB sampling rate, N tops out at ~few thousand on most
// workloads, so even a per-call rebuild is bounded by single-digit
// milliseconds and avoids touching the lock-free Treiber-stack invariants
// in `sampled_list.h`.  The trade-off matters because the lookup itself
// is by definition an out-of-band, off-the-hot-path operation (driven by
// PMU samples or post-mortem inspection); the work performed at lookup
// time is irrelevant to allocator throughput.
//
// Interior pointers are supported: a query address falling anywhere
// inside [base_addr, base_addr + allocated_size) matches.  A pointer
// outside every live sampled range yields std::nullopt.
//
// Concurrency: the snapshot walk uses the existing lock-free
// `SampledList::snapshot` API -- concurrent allocs and frees mid-walk
// are tolerated by construction (linearisable against the tombstone
// CAS).  We never mutate the SampledList from this code path.

#pragma once

#include "../ds_core/defines.h"
#include "sampled_alloc.h"
#include "sampler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace snmalloc::profile
{
  /**
   * Frames returned by `lookup_alloc_site`.  A fixed-size inline array of
   * captured return addresses -- innermost first -- plus an explicit
   * `depth` so the caller knows how many entries are populated.
   *
   * The array length matches `MaxStackFrames` (= `SNMALLOC_PROFILE_STACK_FRAMES`)
   * so the layout mirrors what a SampledAlloc actually stores; no
   * truncation happens on the C++ side.  Frames beyond `depth` are
   * undefined (typically zero).
   */
  struct LookupFrames
  {
    /// Captured return addresses, innermost first.
    std::array<uintptr_t, MaxStackFrames> frames{};
    /// Number of valid entries in `frames` (0..=MaxStackFrames).
    size_t depth{0};
    /// Base address of the matched allocation (start of the live range).
    /// Useful for callers that received an *interior* address and want
    /// to know how far into the object the original PMU sample landed.
    uintptr_t base_addr{0};
    /// Sizeclass-rounded size of the matched allocation.  Together with
    /// `base_addr` this lets callers reconstruct the live byte range.
    size_t allocated_size{0};
  };

  /**
   * Look up `addr` in the global live-sample list.
   *
   * Returns the originating allocation's captured stack iff:
   *   - the allocation was selected by the Poisson sampler, and
   *   - the allocation is still live at the moment of this call, and
   *   - `addr` falls inside `[base, base + allocated_size)`.
   *
   * Returns `std::nullopt` otherwise -- including for any address that
   * lives in a non-sampled allocation (the common case under the default
   * 1-in-512KiB sampling rate).
   *
   * Concurrent allocs/frees are tolerated by the underlying lock-free
   * SampledList snapshot; a sample that fires after this call starts may
   * or may not be observed, and a sample that is freed mid-walk may or
   * may not be observed -- both outcomes are correct for a heap-profiler
   * reverse lookup.
   */
  [[nodiscard]] inline std::optional<LookupFrames>
  lookup_alloc_site(uintptr_t addr) noexcept
  {
    // Materialise a sorted-by-base view of the currently-live samples.
    // We store (base, allocated_size, node*) triples so the binary search
    // below can do range containment without re-deriving sizes from the
    // node, and so we can copy the stack out *after* the search picks a
    // winner (avoids copying frames we will not use).
    struct Entry
    {
      uintptr_t base;
      size_t size;
      const SampledAlloc* node;
    };

    // Reserve a sensible initial capacity; the global list's debug_count
    // call is itself an O(N) walk so we just push into the vector and let
    // it grow.  Heap-allocate via the libc allocator (`std::vector` uses
    // the global new/delete, which snmalloc replaces transparently when
    // it is the process allocator) -- this is fine because lookup is by
    // construction off the alloc hot path.
    std::vector<Entry> entries;

    SamplerGlobals::list().snapshot(
      [&](SampledAlloc* node) noexcept {
        // Skip pathological zero-size entries: every live SampledAlloc
        // must carry a positive allocated_size (the sampler asserts on
        // size_to_sizeclass), but a defensive check costs nothing here
        // and keeps the bound `[base, base + size)` half-open in the
        // strict sense.
        if (node->allocated_size == 0)
          return;
        entries.push_back(Entry{
          node->alloc_addr, node->allocated_size, node});
      });

    if (entries.empty())
      return std::nullopt;

    // Sort by base address ascending.  Stable order is irrelevant -- we
    // only care that binary-search containment works, and live samples
    // cannot have overlapping ranges (an address belongs to exactly one
    // live allocation at any instant; concurrent dealloc + realloc
    // through the same address is fine because we operate on a snapshot).
    std::sort(
      entries.begin(),
      entries.end(),
      [](const Entry& a, const Entry& b) noexcept {
        return a.base < b.base;
      });

    // Binary search: find the greatest base <= addr, then check the
    // half-open range [base, base + size).  std::upper_bound gives us
    // the first base > addr; the candidate is its predecessor.
    auto it = std::upper_bound(
      entries.begin(),
      entries.end(),
      addr,
      [](uintptr_t needle, const Entry& e) noexcept {
        return needle < e.base;
      });

    if (it == entries.begin())
      return std::nullopt; // addr precedes every live sample's base.

    --it;
    const Entry& cand = *it;
    if (addr >= cand.base + cand.size)
      return std::nullopt; // gap between samples.

    // Copy the frames out into the result.  Bounded by MaxStackFrames at
    // both source and destination so a malformed `stack_depth` value
    // cannot cause an out-of-bounds read.
    LookupFrames out;
    const size_t depth = cand.node->stack_depth <= MaxStackFrames
      ? cand.node->stack_depth
      : MaxStackFrames;
    out.depth = depth;
    out.base_addr = cand.base;
    out.allocated_size = cand.size;
    for (size_t i = 0; i < depth; ++i)
      out.frames[i] = cand.node->stack[i];
    return out;
  }
} // namespace snmalloc::profile
