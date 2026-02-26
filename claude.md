# Claude AI Guidelines for snmalloc

## Working Style

**Complete the plan, then check in**: When a plan is approved, execute all
steps to completion. Don't stop after each step for review. When you think
you're done, recursively apply all relevant principles from this file — check
each one, act on any that apply, then check again until no more principles
are relevant. Only then report completion and wait for feedback.

**Plans require discussion before implementation**: After devising a plan
(whether in plan mode or not), run the review loop (see "Mandatory review
checkpoints") before presenting it. Do NOT proceed to implementation until
the plan has been seen and explicitly approved.

**Mandatory review checkpoints**: At each of these points, run the full
review loop — spawn a fresh-context reviewer subagent, address findings,
spawn another fresh reviewer, repeat until a reviewer finds no issues. When
you disagree with a reviewer's finding, escalate — do not resolve disputes
unilaterally. Do not proceed past a checkpoint without a clean review.
1. **After devising a plan**, before presenting it for discussion. For plan
   reviews, adapt the reviewer prompt: instead of reading changed files and
   running tests, the reviewer should read the plan document, read existing
   code the plan references, verify assumptions about the codebase, and check
   for structural gaps (missing steps, naming conflicts, incorrect
   dependencies).
2. **After completing implementation and self-review**, before opening a PR.

The only exception: if you believe a change is truly trivial (a typo fix, a
one-line config change), ask for permission to skip the review. Do not decide
on your own that something is trivial enough to skip. When in doubt, run the
review.

**Go slow to go fast**: Before starting implementation work, identify and state
which principles from these instructions are most relevant to the current task.
This surfaces the right guidelines before they're needed rather than
rediscovering them after a mistake.

**Challenge me when the evidence says I'm wrong**: If a reviewer flags something
that contradicts what I said, or if you have concrete evidence that an
instruction is incorrect, raise it — don't silently comply. Present the evidence
and discuss it.

**Research findings belong in the plan**: If research or exploration surfaces
issues beyond the original task (inaccurate comments, dead code, related bugs),
include them as explicit plan steps — don't just mention them in the analysis
and move on. Anything worth noting is worth acting on or explicitly deferring.

**Self-review is part of done**: The recursive principle check described in
"Complete the plan, then check in" IS the self-review. It's not a separate
step — it's what "done" means. Never report completion without having done it.

**During reviewer loops**: At any point during the review loop — when fixing
findings, when unsure about a reviewer's suggestion, when making tradeoff
decisions — stop and ask. The automated review removes me as a gatekeeper, not
as a collaborator.

## Debugging Principles

1. **Logging is essential** - When debugging issues in allocator code, add tracing to identify the exact point of failure. Use `write()` directly to stderr/file rather than `printf`/`message` to avoid recursion through the allocator.

2. **New code is most likely at fault** - When tests fail after changes, assume the new code introduced the bug. Don't blame existing infrastructure that was working before.

3. **Baseline against origin/main** - Before assuming a system-wide issue, verify the test passes on `origin/main`. This confirms whether the issue is a regression introduced by your changes.

4. **Check the whole PR for patterns** - When fixing a bug of a specific shape (e.g., "one-armed `if constexpr` causes MSVC C4702"), immediately search all changed files in the PR for the same pattern. Fix all instances at once rather than waiting for CI to report each one individually.

5. **Verify hypotheses before acting** - A hypothesis about a bug's cause is not knowledge — it's a guess. Before investing effort in workarounds or fixes, validate empirically that your suspected cause is actually the cause. Read the code more carefully, write a minimal reproducer, or examine the actual data. Verify first, then act.

6. **CI is the source of truth for build status** - A local build failure does not mean the build is broken. Local toolchain versions, stale dependency caches, and environment differences can all cause local failures that don't reproduce in CI. Never declare a build "broken on main" based on local results — check CI first.

## Code Quality

- **Use cross-platform macros from `defines.h`** - Never use raw compiler attributes like `__attribute__((used))` or `__forceinline` directly. Instead use the corresponding `SNMALLOC_*` macros (e.g., `SNMALLOC_USED_FUNCTION`, `SNMALLOC_FAST_PATH`, `SNMALLOC_SLOW_PATH`, `SNMALLOC_PURE`, `SNMALLOC_COLD`, `SNMALLOC_UNUSED_FUNCTION`, `ALWAYSINLINE`, `NOINLINE`). These are defined in `ds_core/defines.h` with correct expansions for MSVC, GCC, and Clang.

- **Don't encode platform assumptions** - Avoid hardcoding limits like "48-bit address space" or "256 TiB max allocation". These assumptions may not hold on future platforms (56-bit, 64-bit address spaces, CHERI, etc.).

- **Trust the existing bounds checks** - snmalloc already has appropriate bounds checking at API boundaries. New internal code should defer to the backend for edge cases rather than adding redundant checks.

- **Guard new data structures** - When adding caches or intermediate layers, ensure they handle all input ranges correctly, including sizes larger than what they cache. Return early/bypass for out-of-range inputs.

- **Keep headers minimal** - Each header should only include what it directly needs. Avoid adding transitive includes "for convenience" — if a header's own declarations only need `<stdint.h>`, don't pull in heavier internal headers. Includers are responsible for their own dependencies. This keeps compile times low and dependency graphs clean.

- **No C++ STL or C++ standard library headers** - snmalloc must be compilable as part of a libc implementation, so it cannot depend on an external C++ STL. Never use headers like `<cstdint>`, `<cstddef>`, `<type_traits>`, `<atomic>`, etc. directly. Instead use the C equivalents (`<stdint.h>`, `<stddef.h>`) or snmalloc's own STL wrappers in `src/snmalloc/stl/` (e.g., `snmalloc/stl/type_traits.h`, `snmalloc/stl/atomic.h`, `snmalloc/stl/array.h`). These wrappers have both a `gnu/` backend (no C++ STL dependency) and a `cxx/` backend, selected at build time.

- **Prefer explicit over implicit** - Avoid relying on implicit conversions, convention-based wiring, or unnamed dependencies. A few extra characters of explicit code is almost always cheaper than someone later needing to reconstruct the hidden knowledge. This is especially relevant in C++ with its many implicit conversion paths and template magic.

- **Document coupling at the point of breakage** - When code A depends on the internal behaviour of code B (read sequence, execution order, size assumptions), put the comment on B — that's where a future maintainer would make a breaking change. Commenting at A doesn't help because the person changing B won't be reading A.

- **Design for changeability, not for predicted changes** - Make designs modular and replaceable so future needs can be accommodated, but don't add abstractions, extension points, or features for changes that haven't happened yet. The goal is a design that's easy to modify, not one that anticipates specific modifications.

## Code Change Discipline

- **Read before modifying** - Do not propose changes to code you haven't read. Understand existing code before suggesting modifications.

- **Prefer editing over creating** - Edit existing files rather than creating new ones. This prevents file bloat and builds on existing work.

- **Avoid over-engineering** - Only make changes that are directly requested or clearly necessary. Don't add error handling for scenarios that can't happen. Don't add docstrings or comments to code you didn't change. Don't create helpers or abstractions for one-time operations. Three similar lines of code is better than a premature abstraction.

- **Evaluate copied patterns, don't cargo-cult** - When reusing a pattern from existing snmalloc code, evaluate each choice (`constexpr` vs runtime, template vs function parameter, etc.) in the context of the new usage. The original may have had reasons that don't apply, or it may have been a mistake. Copy the intent, not the incidental choices. Conventions (legal headers, naming schemes, file organisation) should be followed for consistency; technical patterns should be evaluated on merit.

- **Fix what your change makes stale** - When a change invalidates something elsewhere — a comment, a test description, documentation — fix it in the same PR. Stale artefacts left behind are bugs in the making, and "I didn't modify that line" isn't an excuse when your change is what made it wrong.

## Testing

- Run `func-malloc-fast` and `func-jemalloc-fast` to catch allocation edge cases
- The `-check` variants include assertions but may pass when `-fast` hangs due to timing differences
- Use `timeout` when running tests to avoid infinite hangs
- Before considering a change complete, run the full test suite: `ctest --output-on-failure -j 4 -C Release --timeout 60`

### Test library (`snmalloc_testlib`)

Tests that only use the public allocator API can link against a pre-compiled static library (`snmalloc-testlib-{fast,check}`) instead of compiling the full allocator in each TU.

- **Header**: `test/snmalloc_testlib.h` — forward-declares the API surface; does NOT include any snmalloc headers. Tests that also need snmalloc internals (sizeclasses, pointer math, etc.) include `<snmalloc/snmalloc_core.h>` or `<snmalloc/pal/pal.h>` alongside it.
- **CMake**: Add the test name to `LIBRARY_FUNC_TESTS` or `LIBRARY_PERF_TESTS` in `CMakeLists.txt`.
- **Apply broadly**: When adding new API to testlib (e.g., `ScopedAllocHandle`), immediately audit all remaining non-library tests to see which ones can now be migrated. Don't wait for CI to find them one by one.
- **Cannot migrate**: Tests that use custom `Config` types, `Pool<T>`, override machinery, internal data structures (freelists, MPSC queues), or the statically-sized `alloc<size>()` template with many size values genuinely need `snmalloc.h`.

## Build

- Build directory: `build/`
- Build system: Ninja with CMake
- Rebuild all targets before running ctest: `ninja -C build` (required - ctest runs pre-built binaries)
- Rebuild specific targets: `ninja -C build <target>`
- Always run `clang-format` before committing changes: `ninja -C build clangformat`

## Benchmarking

- Before benchmarking, verify Release build: `grep CMAKE_BUILD_TYPE build/CMakeCache.txt` should show `Release`
- Debug builds have assertions enabled and will give misleading performance numbers
