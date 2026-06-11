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

#ifdef __cplusplus
}
#endif
