// SPDX-License-Identifier: MIT
//
// Heap profiler -- C ABI surface for Rust consumers (and any other FFI
// caller). Phase 4.0 of the heap-profiling milestone: declarations only,
// no policy/wrapper logic.
//
// The symbols are ALWAYS exported (and ALWAYS linkable) regardless of
// whether the C++ build was configured with SNMALLOC_PROFILE=ON.  When the
// flag is OFF every function except `sn_rust_profile_supported` is a
// trivial no-op / returns 0 / nullptr.  This keeps the FFI surface stable
// so a single snmalloc-sys crate can be built against either flavour
// without #[cfg] gating in the Rust crate's extern blocks.
//
// Stack-frame depth captured per sample is SNMALLOC_PROFILE_STACK_FRAMES,
// the same constant the C++ profile subsystem uses.  Default 32 (see
// src/snmalloc/profile/sampled_alloc.h).  Keeping the two in lockstep is
// an ABI invariant: if you bump SNMALLOC_PROFILE_STACK_FRAMES in
// sampled_alloc.h you MUST rebuild snmalloc-sys.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SNMALLOC_PROFILE_STACK_FRAMES
#  define SNMALLOC_PROFILE_STACK_FRAMES 32
#endif

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One sampled allocation, copied out of the in-process SampledList by
 * sn_rust_profile_snapshot_get.  The layout is a plain C struct so the
 * Rust side can mirror it verbatim with `#[repr(C)]`.
 *
 * Fields:
 *   alloc_ptr        Pointer returned by the original alloc.  May be null
 *                    if the alloc-side hook could not record one (rare).
 *   requested_size   Size requested by the caller (bytes).
 *   allocated_size   Size actually returned by snmalloc (sizeclass-rounded).
 *   weight           Bytes-of-request weight for this sample (Poisson
 *                    unbiased estimator -- see profile-weight.md).
 *   stack_depth      Number of valid entries in `stack` (0..=
 *                    SNMALLOC_PROFILE_STACK_FRAMES).
 *   stack            Captured return addresses, innermost first.  Entries
 *                    beyond `stack_depth` are unspecified.
 */
struct SnRustProfileRawSample
{
  void* alloc_ptr;
  size_t requested_size;
  size_t allocated_size;
  size_t weight;
  uint32_t stack_depth;
  void* stack[SNMALLOC_PROFILE_STACK_FRAMES];
};

/**
 * Returns true iff this build of snmalloc was compiled with
 * SNMALLOC_PROFILE=ON.  When false, every other sn_rust_profile_* call is
 * a no-op (or returns zero) and a Rust caller should not bother allocating
 * a snapshot.
 */
SNMALLOC_EXPORT bool sn_rust_profile_supported(void);

/**
 * Set the mean sampling interval, in bytes.  0 disables sampling.
 *
 * When SNMALLOC_PROFILE=OFF this is a no-op.
 */
SNMALLOC_EXPORT void sn_rust_profile_set_sampling_rate(size_t bytes);

/**
 * Get the current mean sampling interval, in bytes.
 *
 * When SNMALLOC_PROFILE=OFF returns 0.
 */
SNMALLOC_EXPORT size_t sn_rust_profile_get_sampling_rate(void);

/**
 * Begin a snapshot of the currently-live sampled allocations.  Returns an
 * opaque handle that can be passed to sn_rust_profile_snapshot_count /
 * sn_rust_profile_snapshot_get.  The caller MUST eventually pass the
 * handle to sn_rust_profile_snapshot_end to release the backing storage.
 *
 * A null return value indicates either that profiling is disabled
 * (SNMALLOC_PROFILE=OFF) or that the snapshot allocation itself failed.
 * Callers should treat both cases as "no samples".
 *
 * Concurrent allocs/frees during the snapshot are tolerated by the
 * SampledList's lock-free design; a sample that begins after begin() may
 * or may not appear, and a sample that ends after begin() may or may not
 * appear -- both outcomes are correct for a heap profiler.
 */
SNMALLOC_EXPORT void* sn_rust_profile_snapshot_begin(void);

/**
 * Number of samples in the snapshot identified by `handle`.  Returns 0
 * for a null handle or when SNMALLOC_PROFILE=OFF.
 */
SNMALLOC_EXPORT size_t sn_rust_profile_snapshot_count(void* handle);

/**
 * Copy sample at index `idx` into `*out`.  Returns true on success,
 * false when:
 *   - SNMALLOC_PROFILE=OFF (no samples to copy)
 *   - handle is null
 *   - out is null
 *   - idx is out of range
 */
SNMALLOC_EXPORT bool
sn_rust_profile_snapshot_get(void* handle, size_t idx, struct SnRustProfileRawSample* out);

/**
 * Release the snapshot allocated by sn_rust_profile_snapshot_begin.
 * Safe to call with a null handle (no-op).
 */
SNMALLOC_EXPORT void sn_rust_profile_snapshot_end(void* handle);

// ---------------------------------------------------------------------------
// Streaming mode (Phase 5.1).
//
// Snapshot mode (above) lets a caller poll the currently-live sampled
// allocations on demand.  Streaming mode is layered on top: a registered
// C callback receives one event per sampled allocation, *as it happens*,
// on the allocating thread.  Mirrors tcmalloc's
// MallocExtension::SetSampleHandler.
//
// Lifecycle:
//   sn_rust_profile_streaming_start(cb)
//     Register `cb` as the active sample handler.  Returns 0 on success,
//     -1 if a handler is already registered (call _stop first) or if
//     `cb` is null.  When SNMALLOC_PROFILE=OFF, returns -1 unconditionally.
//
//   sn_rust_profile_streaming_stop()
//     Unregister the currently-active sample handler.  Returns 0 on
//     success, -1 if no handler is registered.  When SNMALLOC_PROFILE=OFF,
//     returns -1 unconditionally.
//
// Handler invariants (REQUIRED of the caller):
//   - Must be marked `noexcept` (any exception escaping is undefined
//     behaviour).
//   - Must NOT allocate via the snmalloc-managed heap (would attempt to
//     re-enter the sampler; the sampler self-protects against this so
//     the worst case is missed nested samples, but the alloc itself
//     still pays the slow-path cost).
//   - Must complete promptly: the handler runs inline with the sampler
//     slow path on the allocating thread.  Treat it as if it were a
//     signal handler.
//   - The `SnRustProfileRawSample` pointer is valid only for the
//     duration of the call; copy out anything you need.
//
// Streaming and snapshot modes are NOT mutually exclusive: a process may
// register a streaming handler and still call sn_rust_profile_snapshot_*.
// Each sampled allocation is delivered to the streaming handler exactly
// once (alloc-only, no dealloc broadcast -- matches tcmalloc semantics).
// ---------------------------------------------------------------------------

/**
 * Register a streaming sample-handler callback.  Returns 0 on success,
 * -1 on failure (already registered, callback is null, or profiling
 * disabled at build time).
 */
SNMALLOC_EXPORT int sn_rust_profile_streaming_start(
  void (*cb)(const struct SnRustProfileRawSample*));

/**
 * Unregister the currently-active streaming sample handler.  Returns 0
 * on success, -1 if no handler is registered or profiling is disabled
 * at build time.
 */
SNMALLOC_EXPORT int sn_rust_profile_streaming_stop(void);

#ifdef __cplusplus
}
#endif
