#pragma once

// SPDX-License-Identifier: MIT
//
// Backend fragmentation counters (Phase 9.4).
//
// Exposes three OS-level memory-accounting figures that the
// `FullAllocStats` getter (`src/snmalloc/global/stats_export.h`)
// surfaces across the C / Rust FFI boundary:
//
//   bytes_mapped              -- bytes the allocator currently has a
//                                mapping for (i.e.  reserved address
//                                space backed by the parent of the
//                                CommitRange).
//
//   bytes_committed           -- bytes currently in the "in use" state
//                                from the PAL's perspective; on POSIX
//                                that means pages we've MADV_FREE'd-out
//                                of via `notify_using` and not yet
//                                released via `notify_not_using`.
//
//   bytes_decommitted_to_os   -- cumulative number of bytes the
//                                allocator has handed back to the OS
//                                via `PAL::notify_not_using` since
//                                process start.  Strictly monotone.
//
// `bytes_mapped` mirrors the same `StatsRange` accounting that backs
// the legacy `memory_stats()` getter -- the two views differ only in
// units (live OS reservation vs. live OS reservation), so this header
// reads it through `Alloc::Config::Backend::get_current_usage()` at
// the export site rather than maintaining a second counter.  The two
// other figures are owned by this header: `commitrange.h` increments
// the atomics from inside its `notify_using` / `notify_not_using`
// branches.
//
// All counters are `stl::Atomic<size_t>`.  The backend path is not the
// hot path (commit calls hit the PAL, which already issues a syscall
// on most platforms), so the atomics introduce negligible overhead.
//
// Inline-definition `static` data members keep the symbols header-only
// and avoid a new .cc file in the build graph; the linker collapses
// the multiple TU definitions to one shared instance.

#include "snmalloc/stl/atomic.h"

#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
  /**
   * POD snapshot of the backend fragmentation counters.  Returned by
   * `get_backend_frag_stats()`; populated by the FullAllocStats getter
   * in `src/snmalloc/override/stats_export.cc`.
   *
   * All fields are u64 to match the wire format of
   * `struct snmalloc_full_stats`; the underlying atomics are
   * `size_t`-typed but the cast is safe on every platform snmalloc
   * supports (size_t is at most 64 bits).
   */
  struct BackendFragStats
  {
    /** Bytes the allocator currently has committed via the PAL. */
    uint64_t bytes_committed;
    /** Cumulative bytes returned to the OS via `notify_not_using`. */
    uint64_t bytes_decommitted_to_os;
  };

  /**
   * Process-global counter storage for the backend fragmentation
   * accounting.  The struct itself is never instantiated; the static
   * inline members let the counters live in a single linkage unit
   * regardless of how many `CommitRange<PAL>` template instantiations
   * the build emits.
   *
   * `commitrange.h` is the only writer; this header is the only
   * reader.  Atomic updates use `memory_order_relaxed` -- the counters
   * are not used for synchronisation, only for reporting.
   */
  struct BackendFragCounters
  {
    static inline stl::Atomic<size_t> bytes_committed{0};
    static inline stl::Atomic<size_t> bytes_decommitted_to_os{0};

    /**
     * Record a successful `notify_using` of `size` bytes.  Called from
     * `CommitRange<PAL>::alloc_range` after the PAL hands the pages
     * back as in-use.
     */
    static void on_commit(size_t size)
    {
      bytes_committed.fetch_add(size, stl::memory_order_relaxed);
    }

    /**
     * Record a `notify_not_using` of `size` bytes.  Called from
     * `CommitRange<PAL>::dealloc_range` after the PAL has been told to
     * release the pages.  Decreases the live `bytes_committed` figure
     * (clamped at zero to stay defensive against any future caller
     * that double-frees) and bumps the cumulative
     * `bytes_decommitted_to_os` counter.
     */
    static void on_decommit(size_t size)
    {
      // Defensive clamped subtract.  `fetch_sub` of `size` would
      // underflow if `bytes_committed < size`; under normal operation
      // that cannot happen (every dealloc matches a prior alloc), but
      // we treat the underflow path as a no-op rather than corrupting
      // the counter.
      auto prev = bytes_committed.load(stl::memory_order_relaxed);
      while (true)
      {
        auto next = (prev >= size) ? (prev - size) : 0;
        if (bytes_committed.compare_exchange_weak(
              prev, next, stl::memory_order_relaxed))
        {
          break;
        }
      }
      bytes_decommitted_to_os.fetch_add(size, stl::memory_order_relaxed);
    }
  };

  /**
   * Read a coherent (per-counter) snapshot of the backend
   * fragmentation accounting.
   *
   * The two atomics are loaded with `memory_order_relaxed` and the
   * snapshot is NOT transactional: a concurrent commit/decommit may
   * cause the returned `bytes_committed` to lag `bytes_decommitted_to_os`
   * by one operation.  Callers that need a strict invariant should
   * sample twice and reconcile, but for telemetry purposes the
   * single-snapshot read is sufficient.
   */
  inline BackendFragStats get_backend_frag_stats()
  {
    return {
      static_cast<uint64_t>(
        BackendFragCounters::bytes_committed.load(stl::memory_order_relaxed)),
      static_cast<uint64_t>(BackendFragCounters::bytes_decommitted_to_os.load(
        stl::memory_order_relaxed))};
  }
} // namespace snmalloc
