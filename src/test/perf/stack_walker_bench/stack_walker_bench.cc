// SPDX-License-Identifier: MIT
//
// Microbenchmark for the snmalloc frame-pointer stack walker
// (Phase 2.1 of the heap-profiling milestone, ClickUp 86ahzwhq5).
//
// Builds a recursive call chain of known depth and invokes
// `snmalloc::profile::DefaultStackWalker::capture()` from the deepest frame.
// Reports total ns, ns/iteration, and ns/frame; in non-smoke, non-Debug,
// non-null-walker runs, asserts ns/frame is under a generous ceiling.
//
// On platforms where the default walker is the no-op `NullStackWalker`
// (Windows, FreeBSD, OpenEnclave, CHERI, etc.) the benchmark still runs
// but reports the no-op cost and skips the per-frame ceiling assertion.

#include <test/opt.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>

// The walker header is self-contained header-only PAL code; including it
// directly here is fine. It does not need anything from snmalloc_core.h.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <snmalloc/ds_core/defines.h> // NOINLINE, snmalloc::Debug
#include <snmalloc/pal/pal_stack_walker.h>
#include <vector>

namespace
{
  // ---- Tunables ---------------------------------------------------------
  // Max captured frames per call. Slightly larger than the production
  // budget (32) so the depth knob isn't silently clipped.
  static constexpr size_t kMaxFrames = 64;

  // Default per-depth iteration counts. Mirrors the layered convention
  // used by other perf tests (externalpointer.cc:88-111).
#if defined(NDEBUG) && !defined(_MSC_VER)
  static constexpr size_t kIterDefault = 1000000;
#elif defined(_MSC_VER)
  static constexpr size_t kIterDefault = 200000;
#else
  static constexpr size_t kIterDefault = 100000;
#endif

  // Depth sweep. Slope of (total_ns vs depth) is the per-frame cost --
  // more stable than any single depth's absolute number.
  static constexpr size_t kDepths[] = {2, 4, 8, 16, 32};
  static constexpr size_t kNumDepths = sizeof(kDepths) / sizeof(kDepths[0]);

  // Repeat each (depth, iters) batch and take the min, for outlier
  // rejection (cf. perf-stat --repeat / llvm-mca convention).
  static constexpr size_t kRepeats = 5;

  // Per-frame ceiling. Design target is ~10 ns/frame; this ceiling gives
  // ~5x headroom for older hardware and CI noise.
  static constexpr double kPerFrameCeilingNs = 50.0;

  // ---- Sinks to keep the optimiser from eliding the work ---------------
  alignas(64) static uintptr_t g_sink[kMaxFrames];
  static volatile size_t g_sink_depth = 0;
  // Captured depth observed from *inside* the recursion (i.e. with all
  // recurse() frames on the stack). Sampled in the warmup pass so the
  // timed loop measures the true stack depth, not the post-return depth.
  static volatile size_t g_last_captured_depth = 0;

  SNMALLOC_FAST_PATH_INLINE void consume(const uintptr_t* frames, size_t depth)
  {
    // XOR-fold every captured frame address into a single sink. This
    // forces the compiler to emit the store of every `out[depth] = pc`
    // inside the walker's inner loop (otherwise it observes that only
    // a leading prefix of `out` is read and dead-store-eliminates the
    // tail, which underestimates per-frame cost).
    uintptr_t acc = depth;
    for (size_t i = 0; i < depth; i++)
    {
      acc ^= frames[i];
    }
    g_sink[0] = acc;
    g_sink_depth = depth;
  }

  using Walker = snmalloc::profile::DefaultStackWalker;
  static constexpr bool kHaveRealWalker =
    Walker::kind == snmalloc::StackWalkerKind::FramePointer;

  // ---- Recursive call-chain builder ------------------------------------
  // NOINLINE on both the recursive function and the leaf is mandatory:
  // with inlining the compiler will collapse the chain into a single frame
  // and we'd measure ~0 ns/frame regardless of depth.
  NOINLINE void recurse(size_t remaining, size_t batch);

  // A volatile pointer to the frames buffer so the compiler cannot prove
  // that nobody but `consume()` reads it -- this forces every
  // `out[depth++] = pc` store inside the walker loop to be retained, so
  // the ns/frame measurement reflects the real production cost.
  static uintptr_t g_frames[kMaxFrames];
  static uintptr_t* volatile g_frames_ptr = g_frames;

  NOINLINE void leaf(size_t batch)
  {
    size_t last_d = 0;
    for (size_t i = 0; i < batch; i++)
    {
      // Read the buffer pointer through a volatile so the compiler must
      // assume the buffer escapes (preventing dead-store elimination of
      // the walker's inner `out[depth] = pc` writes).
      uintptr_t* frames = g_frames_ptr;
      size_t d = Walker::capture(frames, kMaxFrames, /*skip=*/0);
      consume(frames, d);
      last_d = d;
    }
    // Publish the most recent captured depth so callers can observe the
    // walker's view of the stack from *inside* the recursion.
    g_last_captured_depth = last_d;
  }

  NOINLINE void recurse(size_t remaining, size_t batch)
  {
    if (remaining == 0)
    {
      leaf(batch);
      return;
    }
    recurse(remaining - 1, batch);
    // Prevent tail-call optimisation: force a use of `remaining` after
    // the recursive call so the call site cannot become a jump (which
    // would collapse frames in the chain).
#if defined(__GNUC__) || defined(__clang__)
    __asm__ volatile("" : : "r"(remaining) : "memory");
#else
    g_sink_depth ^= remaining;
#endif
  }

  struct Sample
  {
    size_t captured_depth;
    uint64_t elapsed_ns;
  };

  NOINLINE Sample run_one(size_t depth, size_t iters)
  {
    // Warmup at this depth to page in I-cache and let CPU frequency settle.
    // Also captures depth from inside the recursion (see g_last_captured_depth
    // in leaf()), which is the actual stack depth the timed loop measured.
    recurse(depth, std::min<size_t>(iters, 1024));
    size_t actual = g_last_captured_depth;

    auto t0 = std::chrono::steady_clock::now();
    recurse(depth, iters);
    auto t1 = std::chrono::steady_clock::now();

    Sample s;
    s.captured_depth = actual;
    s.elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return s;
  }

  struct DepthResult
  {
    size_t depth;
    size_t captured_depth;
    uint64_t min_ns;
    double ns_per_iter;
    double ns_per_frame;
  };
} // namespace

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);
  bool smoke = opt.has("--smoke");

  std::cout << "stack_walker: " << Walker::name();
  if (!kHaveRealWalker)
  {
    std::cout << " (null walker; per-frame assertion skipped)";
  }
  std::cout << std::endl;

  size_t iters = opt.is<size_t>("--iter", smoke ? 2000 : kIterDefault);
  size_t repeats = opt.is<size_t>("--repeats", smoke ? 1 : kRepeats);

  std::cout << "  iters/batch=" << iters << "  repeats=" << repeats
            << "  ceiling=" << kPerFrameCeilingNs << " ns/frame" << std::endl;

  std::vector<DepthResult> results;
  results.reserve(kNumDepths);

  for (size_t i = 0; i < kNumDepths; ++i)
  {
    size_t depth = kDepths[i];
    uint64_t best_ns = UINT64_MAX;
    size_t captured = 0;
    for (size_t r = 0; r < repeats; r++)
    {
      Sample s = run_one(depth, iters);
      if (s.elapsed_ns < best_ns)
      {
        best_ns = s.elapsed_ns;
        captured = s.captured_depth;
      }
    }

    double ns_per_iter = double(best_ns) / double(iters);
    double ns_per_frame = captured > 0 ? ns_per_iter / double(captured) : 0.0;

    std::cout << "  depth_requested=" << depth << " depth_captured=" << captured
              << " total=" << best_ns << " ns"
              << " ns/iter=" << ns_per_iter << " ns/frame=" << ns_per_frame
              << std::endl;

    DepthResult dr;
    dr.depth = depth;
    dr.captured_depth = captured;
    dr.min_ns = best_ns;
    dr.ns_per_iter = ns_per_iter;
    dr.ns_per_frame = ns_per_frame;
    results.push_back(dr);
  }

  // Threshold assertion. Skipped for:
  //   - smoke runs (too few iters for min-of-repeats to converge)
  //   - Debug builds (no inlining)
  //   - null walker (always returns 0 frames; ns/frame is meaningless)
  if (!smoke && !snmalloc::Debug && kHaveRealWalker)
  {
    const DepthResult& deepest = results.back();
    if (deepest.captured_depth == 0)
    {
      std::cerr << "FAIL: walker returned 0 frames at deepest depth -- "
                << "frame pointers may have been omitted from the build."
                << std::endl;
      return 1;
    }
    if (deepest.ns_per_frame > kPerFrameCeilingNs)
    {
      std::cerr << "FAIL: ns/frame=" << deepest.ns_per_frame
                << " exceeds ceiling of " << kPerFrameCeilingNs
                << " ns/frame at captured_depth=" << deepest.captured_depth
                << std::endl;
      return 1;
    }

    // Two-point slope: per-frame cost computed from the linear-fit of
    // total_ns vs depth between the shallowest and deepest sample.
    const DepthResult& shallow = results.front();
    if (deepest.captured_depth > shallow.captured_depth)
    {
      double slope = (deepest.ns_per_iter - shallow.ns_per_iter) /
        double(deepest.captured_depth - shallow.captured_depth);
      std::cout << "  slope_ns_per_frame=" << slope << std::endl;
      if (slope > kPerFrameCeilingNs)
      {
        std::cerr << "FAIL: slope ns/frame=" << slope << " exceeds ceiling of "
                  << kPerFrameCeilingNs << std::endl;
        return 1;
      }
    }
  }

  return 0;
}
