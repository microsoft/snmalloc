#pragma once

/**
 * Stack-walker primitive used by the heap-profiling subsystem.
 *
 * Phase 2.1 of the heap-profiling milestone (ClickUp 86ahzwhq5).
 *
 * Provides a frame-pointer walker on x86_64 / aarch64 + Linux/macOS, and a
 * null walker fallback for all other targets. The walker is purely additive
 * in this commit: it is NOT yet wired into any allocator path, NOT gated on
 * a profile build flag, and does not alter existing behaviour.
 *
 * Properties of the FP walker:
 *   - Async-signal-safe. No malloc, no locks, no syscalls, no TLS
 *     construction (the per-thread stack-bounds cache is a POD `thread_local`
 *     that zero-inits to "not valid yet").
 *   - Bounded loop with explicit alignment / monotonic-FP / stack-range
 *     validation; degrades gracefully (returns the prefix it walked) when an
 *     FP chain is corrupted or absent.
 *   - On aarch64 strips Pointer-Authentication Code bits from the saved LR
 *     before returning it. The strip is unconditional on aarch64 (the
 *     `xpaclri` HINT decodes to NOP on cores without FEAT_PAuth, so this is
 *     free on non-PAC hardware) -- whether saved LRs carry PAC bits depends
 *     on kernel/userspace state the allocator does not know at compile time.
 *
 * Selection is at compile time via the C/C++ preprocessor only -- no new
 * CMake option in this commit. The default policy is:
 *
 *   - aarch64 / x86_64 on Linux / macOS: frame-pointer walker.
 *   - everything else (Windows, FreeBSD, OpenEnclave, CHERI/Morello, other
 *     archs): null walker that returns 0 frames.
 *
 * A CMake-level `SNMALLOC_PROFILE_STACK_WALKER` override (fp/null/auto) and
 * the matching `-fno-omit-frame-pointer` injection for snmalloc TUs are
 * deferred to a follow-up. See bottom of file for the override hook.
 */

#include "../ds_core/defines.h"
#include "pal_consts.h"

#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Override hooks
// ---------------------------------------------------------------------------
//
// Callers (or a future CMake plumbing layer) may force a specific walker by
// defining one of these before including this header:
//
//   SNMALLOC_PROFILE_STACK_WALKER_FP    -- use the FP walker unconditionally
//   SNMALLOC_PROFILE_STACK_WALKER_NULL  -- use the null walker unconditionally
//
// If neither is set, an "auto" policy picks FP on supported (arch, OS) pairs
// and null elsewhere.

#if !defined(SNMALLOC_PROFILE_STACK_WALKER_FP) && \
  !defined(SNMALLOC_PROFILE_STACK_WALKER_NULL)
#  if (defined(__x86_64__) || defined(__aarch64__)) && \
    (defined(__linux__) || defined(__APPLE__)) && \
    !defined(__CHERI_PURE_CAPABILITY__)
#    define SNMALLOC_PROFILE_STACK_WALKER_FP 1
#  else
#    define SNMALLOC_PROFILE_STACK_WALKER_NULL 1
#  endif
#endif

#if defined(SNMALLOC_PROFILE_STACK_WALKER_FP)
#  if defined(__linux__) || defined(__APPLE__)
#    include <pthread.h>
#  endif
#  if defined(__APPLE__) && __has_include(<ptrauth.h>)
#    include <ptrauth.h>
#  endif
#endif

namespace snmalloc
{
  /**
   * Tag bit advertised by PALs that supply a non-null stack walker.
   *
   * This is a flag value, separate from `PalFeatures`, used by callers that
   * want to opt out gracefully when running on a PAL whose walker is the
   * no-op stub. It is intentionally not folded into `PalFeatures` in this
   * commit -- the walker isn't yet plumbed into any consumer that needs the
   * `pal_supports<>` SFINAE shape, and adding a flag bit there now would
   * be premature.
   */
  enum class StackWalkerKind : uint8_t
  {
    Null = 0,
    FramePointer = 1,
  };

  namespace profile
  {
#if defined(SNMALLOC_PROFILE_STACK_WALKER_FP)

    // -----------------------------------------------------------------
    // PAC-strip helper (aarch64 only; identity on x86_64).
    //
    // Required because saved LRs on aarch64 may carry Pointer-Authentication
    // Code bits in the top of the pointer. Treating them as raw PCs would
    // either crash a downstream symbolicator (e.g. dladdr) or yield bogus
    // addresses. Stripping is unconditional on aarch64 (see file-level
    // comment for rationale).
    // -----------------------------------------------------------------
    SNMALLOC_FAST_PATH_INLINE uintptr_t strip_pac(uintptr_t lr) noexcept
    {
#  if defined(__aarch64__)
#    if defined(__APPLE__) && __has_include(<ptrauth.h>)
      // Apple's canonical API. Works on both arm64 and arm64e; on arm64
      // it is effectively a NOP for unsigned pointers.
      return reinterpret_cast<uintptr_t>(
        ptrauth_strip(reinterpret_cast<void*>(lr), ptrauth_key_return_address));
#    elif defined(__GNUC__) || defined(__clang__)
      // Emit `xpaclri` (HINT #7) via inline asm. Pre-ARMv8.3 cores decode
      // it as NOP; ARMv8.3+ cores strip the PAC bits from x30.
      register uintptr_t x30 __asm__("x30") = lr;
      __asm__("hint #7" /* xpaclri */ : "+r"(x30));
      return x30;
#    else
      // Fallback mask: clear bits [55:48] (top byte + PAC region under TBI).
      // Safe -- on systems without PAC these bits are already zero.
      return lr & ((uintptr_t{1} << 56) - 1);
#    endif
#  else
      return lr;
#  endif
    }

    // -----------------------------------------------------------------
    // Per-thread stack-bounds cache.
    //
    // POD thread_local: zero-initialised, no constructor, no
    // __cxa_thread_atexit registration, no malloc on first access. This is
    // the critical reentrancy-safe property: any TLS that required dynamic
    // initialisation could re-enter the allocator.
    // -----------------------------------------------------------------
    struct StackBounds
    {
      uintptr_t lo;
      uintptr_t hi;
      bool valid;
    };

    namespace detail
    {
      inline thread_local StackBounds tls_bounds = {0, 0, false};

      inline void populate_bounds(StackBounds& b) noexcept
      {
#  if defined(__APPLE__)
        // Darwin returns the high end (stack origin) directly.
        void* hi = pthread_get_stackaddr_np(pthread_self());
        size_t sz = pthread_get_stacksize_np(pthread_self());
        if (hi != nullptr && sz != 0)
        {
          b.hi = reinterpret_cast<uintptr_t>(hi);
          b.lo = b.hi - sz;
          b.valid = true;
        }
#  elif defined(__linux__)
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0)
        {
          void* lo = nullptr;
          size_t sz = 0;
          if (pthread_attr_getstack(&attr, &lo, &sz) == 0)
          {
            b.lo = reinterpret_cast<uintptr_t>(lo);
            b.hi = b.lo + sz;
            b.valid = true;
          }
          pthread_attr_destroy(&attr);
        }
#  else
        b.valid = false;
#  endif
      }
    } // namespace detail

    inline const StackBounds& get_thread_stack_bounds() noexcept
    {
      if (SNMALLOC_LIKELY(detail::tls_bounds.valid))
        return detail::tls_bounds;
      detail::populate_bounds(detail::tls_bounds);
      return detail::tls_bounds;
    }

    /**
     * Invalidate the cached stack bounds for the current thread.
     *
     * Intended for runtimes that switch fibre / ucontext_t stacks under the
     * application (e.g. Boost.Coroutine). Not used internally; exposed for
     * future integration. Idempotent.
     */
    inline void invalidate_thread_stack_bounds() noexcept
    {
      detail::tls_bounds.valid = false;
    }

    // -----------------------------------------------------------------
    // Frame-pointer walker.
    //
    // Contract:
    //   - `out` must have room for at least `max_depth` entries.
    //   - Returns the number of frames written.
    //   - Caller-facing depth zero is the immediate caller of capture()
    //     (i.e. the seed `__builtin_frame_address(0)` already represents
    //     this function's frame; the first iteration yields its caller).
    //   - `skip` peels off this many leading frames before writing into
    //     `out` -- callers typically pass skip=1 to drop the snmalloc
    //     trampoline frame from the recorded trace.
    // -----------------------------------------------------------------
    struct FramePointerWalker
    {
      static constexpr StackWalkerKind kind = StackWalkerKind::FramePointer;

      static constexpr const char* name() noexcept
      {
        return "fp";
      }

      static SNMALLOC_FAST_PATH_INLINE size_t
      capture(uintptr_t* out, size_t max_depth, size_t skip = 0) noexcept
      {
        if (SNMALLOC_UNLIKELY(max_depth == 0))
          return 0;

        const StackBounds& bounds = get_thread_stack_bounds();
        if (SNMALLOC_UNLIKELY(!bounds.valid))
          return 0;

        auto* fp = static_cast<void**>(__builtin_frame_address(0));
        if (SNMALLOC_UNLIKELY(fp == nullptr))
          return 0;

        uintptr_t prev_fp = 0;
        size_t depth = 0;
        size_t skipped = 0;

        // Hard upper bound on iterations to keep the walker bounded even
        // under a pathological FP chain. `max_depth + skip` is the largest
        // number of *useful* iterations we'd ever do; pad it modestly to
        // tolerate degenerate cases without an infinite loop.
        const size_t max_iters = max_depth + skip + 1;
        for (size_t iter = 0; iter < max_iters; ++iter)
        {
          const auto fp_u = reinterpret_cast<uintptr_t>(fp);

          // Validate the [fp, fp + 2*sizeof(void*)) two-word frame:
          //   - within the cached stack range
          //   - strictly above the previous FP (chain grows toward higher
          //     addresses on grows-down stacks; equal/lower means cycle or
          //     corruption)
          //   - pointer-aligned
          if (SNMALLOC_UNLIKELY(
                fp_u < bounds.lo || fp_u + 2 * sizeof(void*) > bounds.hi ||
                fp_u <= prev_fp || (fp_u & (sizeof(void*) - 1)) != 0))
            break;

          void* next_fp_raw = fp[0];
          void* ret_addr = fp[1];

          if (SNMALLOC_UNLIKELY(ret_addr == nullptr))
            break;

          uintptr_t pc = strip_pac(reinterpret_cast<uintptr_t>(ret_addr));

          if (skipped < skip)
          {
            ++skipped;
          }
          else
          {
            out[depth++] = pc;
            if (depth >= max_depth)
              break;
          }

          prev_fp = fp_u;
          fp = static_cast<void**>(next_fp_raw);

          // Canonical bottom-of-stack sentinel: thread entry trampolines
          // (_start, pthread start_thread, clone child entry) zero the
          // saved FP slot to terminate the chain.
          if (fp == nullptr)
            break;
        }

        return depth;
      }
    };

    using DefaultStackWalker = FramePointerWalker;

#else // SNMALLOC_PROFILE_STACK_WALKER_NULL

    /**
     * No-op walker for platforms where we have not yet implemented native
     * stack walking (Windows production path would use
     * `RtlCaptureStackBackTrace`; CHERI/Morello and SGX are not supported).
     */
    struct NullStackWalker
    {
      static constexpr StackWalkerKind kind = StackWalkerKind::Null;

      static constexpr const char* name() noexcept
      {
        return "null";
      }

      static SNMALLOC_FAST_PATH_INLINE size_t
      capture(uintptr_t* out, size_t max_depth, size_t skip = 0) noexcept
      {
        (void)out;
        (void)max_depth;
        (void)skip;
        return 0;
      }
    };

    inline void invalidate_thread_stack_bounds() noexcept {}

    using DefaultStackWalker = NullStackWalker;

#endif

    /**
     * Public free function. Convenience wrapper for callers that don't want
     * to spell out `DefaultStackWalker::capture` and don't otherwise need
     * to pick a walker explicitly.
     */
    SNMALLOC_FAST_PATH_INLINE size_t
    stack_walk(uintptr_t* out, size_t max_depth, size_t skip = 0) noexcept
    {
      return DefaultStackWalker::capture(out, max_depth, skip);
    }

  } // namespace profile
} // namespace snmalloc
