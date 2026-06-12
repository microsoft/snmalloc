// SPDX-License-Identifier: MIT
//
// Phase 9.6 -- human-readable text dump of allocator telemetry.
//
// This header declares the public dump API for the aggregated
// `snmalloc_full_stats` snapshot from Phase 9.1 (and the populated
// wave-2 fields from 9.2 / 9.3 / 9.4 / 9.5).  It is a pure formatter
// over the existing `snmalloc_get_full_stats` C ABI; no new telemetry
// is collected here.  Output is tcmalloc-style: a single header block
// of MALLOC: lines, an optional per-size-class table, and an optional
// lifetime histogram, all separated by `------------------------------`
// rules.
//
// Three entry points are exposed:
//
//   * `snmalloc::dump_stats(FILE*)`           -- write to an open FILE
//                                                stream (C++ only).
//   * `snmalloc::dump_stats_to_string(std::string&)`
//                                             -- write into a C++
//                                                std::string (clears it
//                                                first).
//   * `snmalloc_dump_stats_to_buffer(buf, len)` (in `extern "C"`)
//                                             -- buffer-based FFI form
//                                                for the Rust binding.
//                                                Two-phase: first call
//                                                with NULL/0 returns the
//                                                required size; second
//                                                call writes up to `len`
//                                                bytes and returns the
//                                                total that *would* have
//                                                been written.  Matches
//                                                the snprintf contract.
//
// The C++ overloads internally call the buffer routine, sizing the
// destination via the size-query first.  Keeping the buffer form as
// the single source of truth simplifies FFI -- FILE* pointers do not
// cross extern-"C" cleanly in every host.
//
// All call sites are read-only: they invoke `snmalloc_get_full_stats`
// (which is itself a pure atomic read) and format the result.  No
// allocator state is mutated.

#pragma once

#include <stddef.h>
#include <stdio.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifdef __cplusplus
#  include <string>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Format the current allocator telemetry snapshot into `buf`.
 *
 * Behaves like `snprintf` w.r.t. truncation:
 *   * if `buf` is non-NULL and `buf_len` is large enough, the full
 *     formatted text (including a trailing NUL terminator) is written.
 *   * if `buf_len` is too small, as many bytes as fit are written and
 *     the buffer is NUL-terminated when `buf_len > 0`.
 *   * if `buf` is NULL or `buf_len` is zero, nothing is written.
 *
 * Returns the number of bytes that *would* have been written *not*
 * counting the trailing NUL.  A caller wanting to size the buffer
 * exactly should call once with `(NULL, 0)`, allocate `n + 1` bytes,
 * then call again with the real buffer.
 *
 * The function captures a fresh snapshot via
 * `snmalloc_get_full_stats` at every call; there is no internal
 * caching.  Safe to invoke from any thread at any point in the
 * process lifetime.
 */
SNMALLOC_EXPORT size_t
snmalloc_dump_stats_to_buffer(char* buf, size_t buf_len);

#ifdef __cplusplus
} // extern "C"
#endif

#ifdef __cplusplus
namespace snmalloc
{
  /**
   * Format and write the current allocator telemetry snapshot to
   * `out`.  Convenience wrapper around `snmalloc_dump_stats_to_buffer`
   * that handles temporary-buffer sizing internally.  `out` must be a
   * writable FILE stream; the formatted block is written in one
   * `fwrite` call.  No newline is appended after the final rule.
   *
   * Does nothing when `out` is null.  No allocator state is mutated.
   */
  SNMALLOC_EXPORT void dump_stats(FILE* out);

  /**
   * Format the current allocator telemetry snapshot into `out`.  The
   * string is cleared first and then filled to its exact required
   * length (no trailing NUL; the std::string carries its own
   * terminator).  Useful for testing -- callers can apply golden
   * regex matches against the resulting std::string without touching
   * a temporary file.
   *
   * No allocator state is mutated.
   */
  SNMALLOC_EXPORT void dump_stats_to_string(std::string& out);
} // namespace snmalloc
#endif // __cplusplus
