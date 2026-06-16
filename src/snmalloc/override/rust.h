// SPDX-License-Identifier: MIT
//
// Core C ABI surface for the snmalloc Rust shim.  Mirror of the
// `sn_rust_*` symbols defined in `rust.cc`; this header carries the
// declarations only so that:
//
//   1. `rust.cc` `#include`s this file and the compiler verifies that
//      the definitions agree with the declarations.
//   2. The Rust bindgen pipeline (both the Cargo `build.rs` path and
//      the Bazel `rust_bindgen_library` rule) can point at a single
//      C entry-point header (`wrapper.h`) to generate FFI bindings
//      without having to parse the C++ source.
//
// The matching header for the heap-profiling surface is
// `rust_profile.h`; together they constitute the complete C ABI
// exposed by the snmalloc Rust shim.

#pragma once

#include <stddef.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /**
   * Allocate `size` bytes with the given `alignment`.  Both must satisfy
   * the constraints documented on the Rust side (`alignment` > 0 and a
   * power of two).  Returns NULL on out-of-memory.
   */
  SNMALLOC_EXPORT void* sn_rust_alloc(size_t alignment, size_t size);

  /**
   * Like `sn_rust_alloc` but zero-initialises the returned region.
   */
  SNMALLOC_EXPORT void* sn_rust_alloc_zeroed(size_t alignment, size_t size);

  /**
   * Deallocate the region previously returned by `sn_rust_alloc` /
   * `sn_rust_alloc_zeroed` / `sn_rust_realloc`.  `alignment` and `size`
   * must match the values used at allocation time.
   */
  SNMALLOC_EXPORT void
  sn_rust_dealloc(void* ptr, size_t alignment, size_t size);

  /**
   * Resize the allocation at `ptr` from `old_size` to `new_size` bytes
   * (both with the same `alignment`).  Returns NULL on failure, in which
   * case the original allocation is left intact.
   */
  SNMALLOC_EXPORT void* sn_rust_realloc(
    void* ptr, size_t alignment, size_t old_size, size_t new_size);

  /**
   * Write the current and peak OS-level memory reservation, in bytes,
   * into the two output pointers.  Both must be non-NULL.
   */
  SNMALLOC_EXPORT void
  sn_rust_statistics(size_t* current_memory_usage, size_t* peak_memory_usage);

  /**
   * Return the usable size in bytes of the allocation at `ptr` (i.e.
   * the size class snmalloc rounded up to).  Returns 0 for NULL.
   */
  SNMALLOC_EXPORT size_t sn_rust_usable_size(const void* ptr);

#ifdef __cplusplus
}
#endif
