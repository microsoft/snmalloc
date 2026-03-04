# Building and Testing Skill

This file is the complete reference for building and testing snmalloc.
It is designed to be used by a subagent that has NO context about what
code changes were made — only that it needs to build and verify the
project. This isolation is intentional: test results must be interpreted
without bias from knowing what changed.

## Build

- Build directory: `build/`
- Build system: Ninja with CMake
- Rebuild all targets before running ctest: `ninja -C build` (required — ctest runs pre-built binaries)
- Rebuild specific targets: `ninja -C build <target>`
- Always run `clang-format` before committing changes: `ninja -C build clangformat`

## Testing

- Run `func-malloc-fast` and `func-jemalloc-fast` to catch allocation edge cases
- The `-check` variants include assertions but may pass when `-fast` hangs due to timing differences
- Use `timeout` when running tests to avoid infinite hangs
- Never run a test on a stale build artifact. Rebuild in the same build directory/config before any run or rerun: `ninja -C <build_dir>` for `ctest` runs, or `ninja -C <build_dir> <target>` if you invoke a direct binary. If the rebuild fails, stop and report with the rebuild log.
- Testing skill: keep commands stable (right build dir/config, consistent flags), prefer `ctest -R <name> -C Release --output-on-failure`, and avoid ad-hoc command variants that change coverage or filters.
- Before considering a change complete, run the full test suite: `ctest --output-on-failure -j 4 -C Release --timeout 60`

### Test failures (never hand-wave)

- Never describe a failure as transient without evidence. Treat every failure as actionable until disproven.
- After a rebuild (per the testing rule above) succeeds, rerun the exact failing command twice: Rerun #1 must match the original command (including filters/flags such as `-R`, `-j`, `--timeout`); Rerun #2 may add only `--output-on-failure` if it was missing. No other changes to flags or filters between reruns.
- Required logging bundle for any failure or flake claim: rebuild command plus stdout/stderr; original failing command plus stdout/stderr; both rerun commands plus stdout/stderr; commit/branch; build directory and config (Release/Debug); compiler/toolchain; host OS; env vars/options affecting the run (allocator config, sanitizers, thread count); note if Rerun #2 added `--output-on-failure`.
- Workflow: record failing command/output → rebuild in the same build directory (stop/report if rebuild fails) → two reruns as above → capture all logs/context → check CI status and origin/main baseline → only label a flake with evidence. Report flakes or unresolved failures in a PR comment with logs and CI links.

### Test library (`snmalloc_testlib`)

Tests that only use the public allocator API can link against a pre-compiled static library (`snmalloc-testlib-{fast,check}`) instead of compiling the full allocator in each TU.

- **Header**: `test/snmalloc_testlib.h` — forward-declares the API surface; does NOT include any snmalloc headers. Tests that also need snmalloc internals (sizeclasses, pointer math, etc.) include `<snmalloc/snmalloc_core.h>` or `<snmalloc/pal/pal.h>` alongside it.
- **CMake**: Add the test name to `LIBRARY_FUNC_TESTS` or `LIBRARY_PERF_TESTS` in `CMakeLists.txt`.
- **Apply broadly**: When adding new API to testlib (e.g., `ScopedAllocHandle`), immediately audit all remaining non-library tests to see which ones can now be migrated. Don't wait for CI to find them one by one.
- **Cannot migrate**: Tests that use custom `Config` types, `Pool<T>`, override machinery, internal data structures (freelists, MPSC queues), or the statically-sized `alloc<size>()` template with many size values genuinely need `snmalloc.h`.

## Benchmarking

- Before benchmarking, verify Release build: `grep CMAKE_BUILD_TYPE build/CMakeCache.txt` should show `Release`
- Debug builds have assertions enabled and will give misleading performance numbers

## Subagent protocol

When you are invoked as a testing subagent:

1. **Read this file first.** It is your only reference for how to build and test.
2. **You have no knowledge of what changed.** Do not ask. Do not speculate. Report only what you observe.
3. **Rebuild before testing.** Always run `ninja -C build` before any test invocation. If the rebuild fails, report the failure and stop.
4. **Run the requested tests** (or the full suite if not specified). Use the exact commands from this file.
5. **Report results factually**: which tests passed, which failed, the exact commands you ran, and the full output of any failures. Do not interpret failures in terms of code changes — you don't know what they are.
6. **Never label a failure as transient.** If a test fails, follow the failure protocol above (rebuild + two reruns + logging bundle). Report all evidence.
