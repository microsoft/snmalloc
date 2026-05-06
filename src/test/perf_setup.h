#pragma once

/**
 * Helper for running perf tests under ctest with reduced iteration counts.
 *
 * When ctest invokes a perf test, it passes `--smoke`. Tests that opt in
 * call `perf_iterations()` to choose between their full (`cli_default`)
 * iteration count and a much smaller `smoke_value`. Direct CLI invocation
 * (no `--smoke`) preserves the full iteration count, so manual perf runs
 * are unaffected.
 *
 * Tests on the concurrency-stress exclusion list always receive
 * `cli_default` even when `--smoke` is set — their iteration count is
 * tuned to provoke races, not to spend time, and is not safe to reduce.
 */

#include <cstring>
#include <test/opt.h>

namespace snmalloc_test
{
  /**
   * Concurrency-stress tests that must not be smoked. Their iteration
   * counts are tuned to provoke scheduler interleavings (e.g. raw
   * thread-count multipliers), not to exercise dispatch paths;
   * reducing them silently weakens scheduler-coverage capability.
   *
   * Race finding proper is the job of the TSAN build, not these
   * tests, so perf tests that merely *use* threads (e.g.
   * `perf-contention`, `perf-msgpass`) are smoke-eligible.
   */
  inline bool is_concurrency_stress(const char* test_name)
  {
    static const char* const excluded[] = {
      "perf-contention",
      "perf-large_producer_consumer",
      "perf-lotsofthreads",
      "perf-msgpass",
    };
    for (const char* e : excluded)
    {
      if (std::strcmp(test_name, e) == 0)
        return true;
    }
    return false;
  }

  /**
   * Returns `smoke_value` when running under `--smoke` and `test_name`
   * is not a concurrency-stress test; otherwise returns `cli_default`.
   *
   * `test_name` is the ctest binary name without the flavour suffix
   * (e.g. "perf-singlethread"). It is injected by CMake as the macro
   * `SNMALLOC_TEST_NAME` so each call site reads
   *
   *   perf_iterations(opt, SNMALLOC_TEST_NAME, default_value, smoke_value);
   */
  inline size_t perf_iterations(
    opt::Opt& opt,
    const char* test_name,
    size_t cli_default,
    size_t smoke_value)
  {
    if (!opt.has("--smoke"))
      return cli_default;
    if (is_concurrency_stress(test_name))
      return cli_default;
    return smoke_value;
  }
} // namespace snmalloc_test
