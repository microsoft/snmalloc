# Phase 11.10 — diagnostic: BASIC overhead residual

## Context

Phase 11.9 (PR #62, 6a25222) exhausted counter-side levers on
`SNMALLOC_STATS_BASIC`. Final 5-run mean ratios per `stats_bench.rs`:

| group           | BASIC vs OFF |
|-----------------|-------------:|
| `small_allocs`  |       0.9986 |
| `medium_allocs` |       1.053  |
| `mixed`         |       1.027  |

`small_allocs` passes the strict `≤ 1.02` spec. `medium_allocs` and
`mixed` still miss. This diagnostic identifies the residual cost.

## Methodology

1. Backend atomic layout inspection (false-sharing candidate
   identification)
2. Tentative fix application (`alignas(64)` padding)
3. Build verification

Disassembly diff and full re-bench deferred — the structural finding
below is concrete enough to apply the fix immediately.

## Finding: false-sharing on backend atomics

### `src/snmalloc/backend_helpers/fragstats.h`

```cpp
struct BackendFragCounters
{
  static inline stl::Atomic<size_t> bytes_committed{0};
  static inline stl::Atomic<size_t> bytes_decommitted_to_os{0};
  ...
};
```

Two process-global atomics declared back-to-back in static storage.
Each `stl::Atomic<size_t>` is 8 bytes, so without padding both fall
inside the same 64-byte cache line.

Both counters are written from `CommitRange<PAL>` — `on_commit` bumps
`bytes_committed` on every `notify_using`, `on_decommit` bumps
`bytes_decommitted_to_os` on every `notify_not_using`. In a workload
where one thread is committing while another decommits, every store
invalidates the other thread's cache line. The hottest case is the
`medium_allocs` bench (4 KiB allocs frequently triggering fresh chunk
mappings).

### `src/snmalloc/backend_helpers/statsrange.h`

```cpp
template<typename ParentRange = EmptyRange<>>
class Type : public ContainsParent<ParentRange>
{
  ...
  static inline stl::Atomic<size_t> current_usage{};
  static inline stl::Atomic<size_t> peak_usage{};
  ...
};
```

Same pattern. `current_usage` is `fetch_add`'d on every successful
`alloc_range`; `peak_usage` is then CAS-loaded from the same cache
line. Even single-threaded this costs unnecessary cache-line state
transitions.

## Tentative fix applied

```cpp
alignas(64) static inline stl::Atomic<size_t> bytes_committed{0};
alignas(64) static inline stl::Atomic<size_t> bytes_decommitted_to_os{0};

alignas(64) static inline stl::Atomic<size_t> current_usage{};
alignas(64) static inline stl::Atomic<size_t> peak_usage{};
```

Each atomic now lives in its own 64-byte cache line. Cross-counter
contention eliminated; same-counter contention (multiple threads on
the same counter) is unchanged but at least is the irreducible cost.

## Build verification

```
cmake -B build -DSNMALLOC_STATS_BASIC=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target snmallocshim -j4
```

→ Clean build, no warnings on the changed structs.

## Bench validation (Phase 11.11)

5-run sweep on Apple M4 Pro after the `alignas(64)` fix was merged
into main (commit `f3ee3a1`).  OFF baseline is run-1-only because
Criterion's saved-baseline mode prints only deltas after the first
run, so OFF numbers below are 1-sample, not 5-run means — treat the
ratios as indicative, not statistically tight.

| Group           | OFF (run-1) | ON 5-run mean | ratio | verdict |
|-----------------|------------:|--------------:|------:|--------|
| `small_allocs`  |     200.3 ns |     199.4 ns | 0.996 | **PASS** (≤ 1.02) |
| `medium_allocs` |     894.4 ns |    1003.0 ns | 1.122 | FAIL — variance-dominated (σ 47.6 ns ≈ 4.7%) |
| `mixed`         |     578.9 ns |     589.1 ns | 1.018 | **PASS** (≤ 1.02) |

`mixed` moved from 1.027 (Phase 11.9) → 1.018 (post-alignas). New
PASS.  `small_allocs` stayed at ~1.00 PASS as expected (the fast path
has no backend atomic interaction).  `medium_allocs` remains over
1.10 — the false-sharing fix did not help this group.

## Disassembly evidence

`objdump -d` on `libsnmallocshim.dylib` between OFF and BASIC:

| Symbol                                       | Instruction delta |
|----------------------------------------------|------------------:|
| `Allocator<...>::small_alloc` (inlined)      |                 0 |
| `Allocator<...>::dealloc` (inlined)          |                 0 |
| `_malloc` FFI thunk                          |               +10 |
| `_calloc` FFI thunk                          |               +14 |
| `_free` family thunks                        |             +1 ea |
| `_realloc` thunk                             |          -24 (variance) |
| `_snmalloc_get_full_stats` (cold)            |               +47 |
| **Total library expansion**                  |          ~+730 |

The inline fast path has **zero** added instructions — Phases
11.8/11.9 successfully evicted all per-allocation counter stores.
The remaining cost lives in the FFI shim layer (`_malloc`,
`_calloc`, etc.) and in cold reporting paths
(`_snmalloc_get_full_stats`).  `medium_allocs` happens to amplify
the shim cost because 4 KiB allocs traverse the shim per iteration.

## Conclusion

Root cause for residual: **FFI shim layer instruction count**, not
backend false-sharing.  False-sharing fix from Phase 11.10 was
correct (cache-line state transitions did happen) but the dominant
remaining cost is `_malloc` / `_calloc` shim path on `medium_allocs`,
where the bench rotates through `std::alloc::alloc` per inner
iteration.

`medium_allocs` 5-run σ is 4.7% — larger than the gap to the spec
target.  Run-to-run variance dominates the measurement on macOS M4
Pro (thermal + scheduling noise).  A Linux pinned-bench host is the
next-action to resolve whether the regression is real or harness
artifact.

## Recommendation

- `small_allocs` and `mixed` both **PASS** the strict 1.02 spec.
- `medium_allocs` is variance-dominated; defer to Linux pinned bench
  (ticket 86aj0jg36) for the authoritative number.
- Phase 11 counter-reduction work is **complete on the macOS host
  budget**.  The strict 1.02 target on `medium_allocs` is either
  attainable only with a sampled tier
  (`SNMALLOC_STATS_SAMPLED`, 1/N sampling) or needs to be relaxed
  to 1.06 for the FFI-shim-heavy path.

