# Building and Testing Skill

This file contains the repository-specific guidance for building, testing, and
benchmarking snmalloc.

## Build

- The conventional build directory is `build/`. Commands below use a
  single-config Ninja build. For a multi-config generator, build with
  `cmake --build build --config Debug` and pass `-C Debug` to CTest; use
  `Release` instead when benchmarking.
- Use a Debug build for functional validation so that allocator assertions and
  debug-conditioned checks are enabled. The separate `-check` test flavour
  defines `SNMALLOC_CHECK_CLIENT` in every build configuration. Verify the
  configuration of a single-config build with
  `grep CMAKE_BUILD_TYPE build/CMakeCache.txt`.
- Rebuild the relevant targets before running tests. Use `ninja -C build` for
  the full build or `ninja -C build <target>` for a focused test.
- Format changed code before committing with `ninja -C build clangformat`.

## Testing

- Prefer focused tests while developing:
  `ctest --test-dir build -R <name> --output-on-failure`.
- Run `func-malloc-fast` and `func-jemalloc-fast` when allocator API or
  compatibility behaviour may be affected. The `-check` variants can miss
  timing-dependent hangs that appear in `-fast`.
- Use a timeout for tests that could hang.
- Run the full Debug suite when the breadth or risk of the change warrants it:
  `ctest --test-dir build --output-on-failure -j 4 --timeout 400`.
- Never test a stale binary. Rebuild in the same directory and configuration
  before a run or rerun.

### Test failures

- Treat a failure as actionable until there is evidence otherwise. Do not call
  it transient solely because a rerun passes.
- Preserve the failing command and output, rebuild the same configuration, and
  rerun with materially equivalent options. Record enough environment and
  configuration information to reproduce unresolved or intermittent failures.
- If attribution is unclear, compare with CI and `origin/main`. Report the
  evidence and any remaining uncertainty rather than guessing.

### Test library (`snmalloc_testlib`)

Tests that use only the public allocator API can include the lightweight test
header and be compiled once for both allocator test flavours.

- `src/test/snmalloc_testlib.h`, included as `<test/snmalloc_testlib.h>`,
  declares the supported API without including the full `snmalloc.h` or
  `snmalloc_core.h` allocator headers.
- Add tests whose only allocator-facing snmalloc header is this header to
  `TESTLIB_ONLY_TESTS` in `CMakeLists.txt` so their source is compiled once and
  linked against both `snmalloc-testlib-fast` and `snmalloc-testlib-check`.
- Tests using custom `Config` types, `Pool<T>`, override machinery, internal
  data structures, or many instantiations of `alloc<size>()` require direct
  allocator headers and must not be classified as testlib-only.
- When extending the test-library API, consider whether existing tests can now
  use it, but avoid unrelated migration churn.

## Benchmarking

- Benchmark only an optimized Release build. For a single-config build, verify
  it with `grep CMAKE_BUILD_TYPE <build_dir>/CMakeCache.txt`; for a multi-config
  build, select `Release` explicitly when building and running the benchmark.
- Rebuild the benchmark target before measuring, retain raw results, and report
  the relevant allocator configuration and known methodological limitations.

### Quick performance experiments

For an explicitly exploratory or disposable experiment:

1. Record the revision, dirty state, compiler, host, benchmark options, and
   relevant allocator configuration.
2. Use a dedicated Release build directory, or verify that the selected build
   is Release.
3. Rebuild the specific target and run a smoke check that exercises the
   instrumented path before collecting measurements.
4. Retain raw measurements and report timer resolution, run count, variability,
   and known limitations.

A disposable experiment does not require a full Debug baseline, full test
suite, formatting, or independent review. Experimental code is not merge-ready.
If it will be retained, committed, or submitted, promote it to a normal change
and perform the validation appropriate to its final scope.
