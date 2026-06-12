// SPDX-License-Identifier: MIT
//
// Implementation of the FullAllocStats getter declared in
// `src/snmalloc/global/stats_export.h` (Phase 9.1 scaffold).
//
// This compilation unit is intentionally tiny: it only needs to see the
// `Alloc::Config::Backend` accessors that already back the existing
// `malloc-extensions.cc` and `rust.cc` stats getters.  No allocator
// state is mutated; the call is a pure read.  All non-`bytes_in_use`
// / `peak_bytes_in_use` fields are zeroed via `memset` first, leaving
// the wave-2 tickets free to populate them without touching this file.

#include "../snmalloc.h"
#include "snmalloc/global/stats_export.h"

#include <string.h>

using namespace snmalloc;

extern "C" SNMALLOC_EXPORT void
snmalloc_get_full_stats(struct snmalloc_full_stats* out)
{
  if (out == nullptr)
    return;

  // Zero-fill first so every field that the wave-2 tickets haven't
  // wired up yet reads as zero -- and so the trailing `reserved[]`
  // pool and future-version slots are guaranteed to be all-zero on
  // older producers.
  memset(out, 0, sizeof(*out));

  out->version = SNMALLOC_FULL_STATS_VERSION;

  // Delegate to the existing StatsRange accounting, matching the
  // semantics of `sn_rust_statistics` and `get_malloc_info_v1`.  These
  // are static accessors on the active Config's backend; they read
  // process-global atomic counters.
  out->bytes_in_use =
    static_cast<uint64_t>(Alloc::Config::Backend::get_current_usage());
  out->peak_bytes_in_use =
    static_cast<uint64_t>(Alloc::Config::Backend::get_peak_usage());
}
