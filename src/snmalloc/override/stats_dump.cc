// SPDX-License-Identifier: MIT
//
// Phase 9.6 -- text-dump implementation.
//
// Pure formatter over `snmalloc_get_full_stats` (Phase 9.1).  Output
// shape mirrors tcmalloc's `MallocExtension::GetStats` text:
//
//   ------------------------------------------------
//   MALLOC:    ....... (   ..  MiB) Bytes in use by application
//   MALLOC: +  ....... (   ..  MiB) Bytes committed to OS
//   ... (six MALLOC: lines total)
//   ------------------------------------------------
//   Class   Size       Live  TotalAllocs  TotalDeallocs
//      0      16        230         5012           4782
//   ... (one row per non-empty size class)
//   ------------------------------------------------
//   Lifetime histogram (log2 ns buckets):
//      bucket   range              count
//          0   [1 ns - 2 ns)        ....
//   ... (one row per non-empty bucket)
//   ------------------------------------------------
//
// Empty optional sections (no live size-class data, all-zero lifetime
// histogram) are omitted entirely so a non-profile, non-stats build
// still produces a readable dump.
//
// FFI surface is a single buffer routine `snmalloc_dump_stats_to_buffer`
// that follows snprintf truncation semantics.  The two C++ overloads
// `dump_stats(FILE*)` and `dump_stats_to_string(std::string&)` are
// thin wrappers that handle the size-query + alloc + fill dance
// internally.  Keeping the buffer routine as the single source of
// truth simplifies the Rust binding (FILE pointers do not cross the
// FFI boundary cleanly on every host).

#include "../snmalloc.h"
#include "snmalloc/global/stats_dump.h"
#include "snmalloc/global/stats_export.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

namespace
{
  /// Bookkeeping struct for an in-progress snprintf-style write.
  ///
  /// `buf` may be NULL (in which case `cap` is treated as zero); in
  /// that case `write` still bumps `total` so callers can use
  /// `(NULL, 0)` to size-query.  `written` tracks how many bytes
  /// (not counting the NUL terminator) have actually been deposited
  /// into `buf`; `total` tracks how many bytes *would* have been
  /// written had the buffer been infinite.
  struct WriteCursor
  {
    char* buf;
    size_t cap;
    size_t written;
    size_t total;
  };

  /// Append `fmt`-formatted text to `*cursor`.  Mirrors snprintf:
  /// returns the number of bytes that would have been emitted (so
  /// callers can detect truncation against `cap`).  Always
  /// NUL-terminates `buf` when `cap > 0`.
  static void
  cursor_printf(WriteCursor* cursor, const char* fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    // Reserve one byte for the trailing NUL; vsnprintf's size argument
    // is "buffer length including terminator".
    size_t remaining =
      (cursor->buf != nullptr && cursor->cap > cursor->written)
      ? (cursor->cap - cursor->written)
      : 0;
    int n = vsnprintf(
      cursor->buf != nullptr ? cursor->buf + cursor->written : nullptr,
      remaining,
      fmt,
      args);
    va_end(args);

    if (n < 0)
    {
      // Encoding error.  Treat as zero-byte append; do not advance
      // either counter.  This path is unreachable for the
      // well-formed format strings used below but the defensive
      // branch keeps the routine total-callable.
      return;
    }

    size_t emitted = static_cast<size_t>(n);
    cursor->total += emitted;
    if (cursor->buf != nullptr && remaining > 0)
    {
      // vsnprintf wrote min(emitted, remaining - 1) bytes (+ NUL).
      // The bytes actually in the buffer are bounded by remaining - 1.
      size_t actually_written = emitted < (remaining - 1)
        ? emitted
        : (remaining - 1);
      cursor->written += actually_written;
    }
  }

  /// Render `bytes` in human-readable form (KiB / MiB / GiB).  Uses
  /// fixed-point "%.1f" to match tcmalloc's output column shape.
  /// Writes into `out` which must hold at least 32 bytes.
  static void
  bytes_to_human(uint64_t bytes, char* out, size_t out_cap)
  {
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    double b = static_cast<double>(bytes);
    if (b >= kGiB)
      snprintf(out, out_cap, "%6.1f GiB", b / kGiB);
    else if (b >= kMiB)
      snprintf(out, out_cap, "%6.1f MiB", b / kMiB);
    else if (b >= kKiB)
      snprintf(out, out_cap, "%6.1f KiB", b / kKiB);
    else
      snprintf(out, out_cap, "%6.0f   B", b);
  }

  /// Render a log2-spaced ns range into `out`.  Bucket i covers
  /// [2^i, 2^(i+1)) ns.  At i >= 30 we switch units to ms / s / hr
  /// so the dump stays readable across the whole 32-bucket span.
  static void
  lifetime_range_to_human(unsigned bucket, char* out, size_t out_cap)
  {
    // Lower and upper bounds in nanoseconds.  Avoid uint64_t overflow
    // by capping at 1 << 63.  The histogram caps the last bucket
    // anyway so the visual representation just needs to be useful.
    uint64_t lo = (bucket >= 63u) ? (uint64_t{1} << 63) : (uint64_t{1} << bucket);
    uint64_t hi = (bucket >= 62u) ? (uint64_t{1} << 63) : (uint64_t{1} << (bucket + 1u));

    auto fmt_one = [](uint64_t ns, char* dst, size_t cap)
    {
      if (ns >= 3'600'000'000'000ull)
        snprintf(dst, cap, "%llu hr", static_cast<unsigned long long>(ns / 3'600'000'000'000ull));
      else if (ns >= 1'000'000'000ull)
        snprintf(dst, cap, "%llu s", static_cast<unsigned long long>(ns / 1'000'000'000ull));
      else if (ns >= 1'000'000ull)
        snprintf(dst, cap, "%llu ms", static_cast<unsigned long long>(ns / 1'000'000ull));
      else if (ns >= 1'000ull)
        snprintf(dst, cap, "%llu us", static_cast<unsigned long long>(ns / 1'000ull));
      else
        snprintf(dst, cap, "%llu ns", static_cast<unsigned long long>(ns));
    };

    char lo_str[24];
    char hi_str[24];
    fmt_one(lo, lo_str, sizeof(lo_str));
    fmt_one(hi, hi_str, sizeof(hi_str));
    snprintf(out, out_cap, "[%s - %s)", lo_str, hi_str);
  }

  /// Map a size-class slot index to the byte size it represents.
  /// The 9.3 ticket indexes by `smallsizeclass_t`, so we delegate
  /// to `snmalloc::sizeclass_to_size`.  Out-of-range slots (no
  /// such class on this configuration) return 0.
  static uint64_t sizeclass_slot_to_bytes(unsigned slot)
  {
    if (slot >= snmalloc::NUM_SMALL_SIZECLASSES)
      return 0;
    return static_cast<uint64_t>(snmalloc::sizeclass_to_size(
      static_cast<snmalloc::smallsizeclass_t>(slot)));
  }

  /// Core formatter.  Writes the dump into `cursor`; uses NULL/0 for
  /// size-querying.  All input data comes from a fresh
  /// `snmalloc_get_full_stats` snapshot.
  static void
  format_dump(WriteCursor* cursor, const snmalloc_full_stats* s)
  {
    char human[32];

    cursor_printf(cursor,
      "------------------------------------------------\n");

    bytes_to_human(s->bytes_in_use, human, sizeof(human));
    cursor_printf(cursor,
      "MALLOC:   %12llu (%s) Bytes in use by application\n",
      static_cast<unsigned long long>(s->bytes_in_use), human);

    bytes_to_human(s->peak_bytes_in_use, human, sizeof(human));
    cursor_printf(cursor,
      "MALLOC: + %12llu (%s) Peak bytes in use\n",
      static_cast<unsigned long long>(s->peak_bytes_in_use), human);

    bytes_to_human(s->bytes_committed, human, sizeof(human));
    cursor_printf(cursor,
      "MALLOC: + %12llu (%s) Bytes committed to OS\n",
      static_cast<unsigned long long>(s->bytes_committed), human);

    bytes_to_human(s->bytes_decommitted_to_os, human, sizeof(human));
    cursor_printf(cursor,
      "MALLOC: + %12llu (%s) Bytes decommitted (returned to OS)\n",
      static_cast<unsigned long long>(s->bytes_decommitted_to_os), human);

    cursor_printf(cursor,
      "MALLOC:   %12llu              Fast-path allocations\n",
      static_cast<unsigned long long>(s->fast_path_allocs));

    cursor_printf(cursor,
      "MALLOC:   %12llu              Slow-path allocations\n",
      static_cast<unsigned long long>(s->slow_path_allocs));

    cursor_printf(cursor,
      "MALLOC:   %12llu              Fast-path deallocations\n",
      static_cast<unsigned long long>(s->fast_path_deallocs));

    cursor_printf(cursor,
      "MALLOC:   %12llu              Cross-thread deallocations\n",
      static_cast<unsigned long long>(s->remote_deallocs));

    cursor_printf(cursor,
      "MALLOC:   %12llu              Message-queue drains\n",
      static_cast<unsigned long long>(s->message_queue_drains));

    cursor_printf(cursor,
      "MALLOC:   %12llu              Cross-thread messages received\n",
      static_cast<unsigned long long>(s->cross_thread_messages_received));

    // --- Per-size-class table (optional) -----------------------------
    //
    // Emit a row for each class whose Live, TotalAllocs, or
    // TotalDeallocs counter is non-zero.  Skips the whole section
    // when every class is empty -- this matters in non-stats builds
    // where the 9.3 instrumentation is compiled out and every slot
    // is zero.
    bool any_class = false;
    for (unsigned i = 0; i < SNMALLOC_FULL_STATS_SIZECLASS_SLOTS; ++i)
    {
      if (s->total_live_count_by_class[i] != 0 ||
          s->cumulative_alloc_by_class[i] != 0 ||
          s->cumulative_dealloc_by_class[i] != 0)
      {
        any_class = true;
        break;
      }
    }
    if (any_class)
    {
      cursor_printf(cursor,
        "------------------------------------------------\n");
      cursor_printf(cursor,
        "Class   Size         Live    TotalAllocs    TotalDeallocs\n");
      for (unsigned i = 0; i < SNMALLOC_FULL_STATS_SIZECLASS_SLOTS; ++i)
      {
        if (s->total_live_count_by_class[i] == 0 &&
            s->cumulative_alloc_by_class[i] == 0 &&
            s->cumulative_dealloc_by_class[i] == 0)
          continue;
        uint64_t bytes = sizeclass_slot_to_bytes(i);
        cursor_printf(cursor,
          "%5u  %5llu  %11llu  %13llu  %15llu\n",
          i,
          static_cast<unsigned long long>(bytes),
          static_cast<unsigned long long>(s->total_live_count_by_class[i]),
          static_cast<unsigned long long>(s->cumulative_alloc_by_class[i]),
          static_cast<unsigned long long>(s->cumulative_dealloc_by_class[i]));
      }
    }

    // --- Lifetime histogram (optional) -------------------------------
    //
    // Emit a row per non-zero bucket, with a human-readable [lo - hi)
    // range.  Skips entirely when all buckets are zero (non-profile
    // builds, or no sampled alloc has yet completed its lifecycle).
    bool any_bucket = false;
    for (unsigned i = 0; i < SNMALLOC_FULL_STATS_LIFETIME_BUCKETS; ++i)
    {
      if (s->lifetime_buckets_ns[i] != 0)
      {
        any_bucket = true;
        break;
      }
    }
    if (any_bucket)
    {
      cursor_printf(cursor,
        "------------------------------------------------\n");
      cursor_printf(cursor,
        "Lifetime histogram (log2 ns buckets):\n");
      cursor_printf(cursor,
        "  bucket  range                       count\n");
      char range[48];
      for (unsigned i = 0; i < SNMALLOC_FULL_STATS_LIFETIME_BUCKETS; ++i)
      {
        if (s->lifetime_buckets_ns[i] == 0)
          continue;
        lifetime_range_to_human(i, range, sizeof(range));
        cursor_printf(cursor,
          "  %6u  %-26s %12llu\n", i, range,
          static_cast<unsigned long long>(s->lifetime_buckets_ns[i]));
      }
    }

    cursor_printf(cursor,
      "------------------------------------------------\n");
  }
} // namespace

extern "C" SNMALLOC_EXPORT size_t
snmalloc_dump_stats_to_buffer(char* buf, size_t buf_len)
{
  snmalloc_full_stats snap;
  // `snmalloc_get_full_stats` memsets the snapshot before populating
  // populated fields, so it's safe to leave `snap` uninitialised here.
  snmalloc_get_full_stats(&snap);

  WriteCursor cursor{buf, buf_len, 0, 0};
  format_dump(&cursor, &snap);

  // Defensive: even if the caller passed a non-NULL buffer we want
  // it NUL-terminated.  `cursor_printf` already does this on every
  // append via vsnprintf, but if the format string emitted zero
  // bytes (impossible with the layout above, but be safe) the
  // terminator may be missing.
  if (buf != nullptr && buf_len > 0)
  {
    size_t term_idx = cursor.written < buf_len ? cursor.written : buf_len - 1;
    buf[term_idx] = '\0';
  }

  return cursor.total;
}

namespace snmalloc
{
  SNMALLOC_EXPORT void dump_stats(FILE* out)
  {
    if (out == nullptr)
      return;
    // Size-query, alloc, fill, write.  Two calls into the buffer
    // routine -- the C ABI promises identical results across both.
    size_t needed = snmalloc_dump_stats_to_buffer(nullptr, 0);
    // Use std::string as the heap-allocated buffer so its destructor
    // releases the memory on every return path.  `needed + 1` bytes
    // for the trailing NUL.
    std::string buf;
    buf.resize(needed);
    if (needed > 0)
    {
      snmalloc_dump_stats_to_buffer(&buf[0], needed + 1);
    }
    if (!buf.empty())
    {
      fwrite(buf.data(), 1, buf.size(), out);
    }
  }

  SNMALLOC_EXPORT void dump_stats_to_string(std::string& out)
  {
    size_t needed = snmalloc_dump_stats_to_buffer(nullptr, 0);
    out.clear();
    out.resize(needed);
    if (needed > 0)
    {
      snmalloc_dump_stats_to_buffer(&out[0], needed + 1);
    }
  }
} // namespace snmalloc
