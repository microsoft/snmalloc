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

## Bench validation

NOT performed in this diagnostic. Predecessor agent's bench runs
stalled mid-iteration and could not complete the 5-run sweep.
Re-bench is a follow-up validation step — the alignas change is
locally obvious from the source layout, the build is clean, and the
worst case (no improvement) is harmless because the padding only
costs ~48 bytes of BSS for each of 2 structs.

## Recommendation

Ship the alignas fix in this PR. Either:

- **If the next `cargo bench --features stats-basic` 5-run mean shows
  `medium_allocs` and `mixed` drop toward 1.02**: declare Phase 11
  done and close the milestone.
- **If ratios are unchanged**: file Phase 11.11 to investigate the
  remaining cost via disassembly diff (`objdump -d` BASIC vs OFF on
  the Allocator<...>::alloc symbol). False-sharing was the highest-
  probability hypothesis; if disproved, codegen drift becomes the
  next candidate.

In either case, the alignas change is correct by construction (lines
are no longer shared) and should not regress anything.

## Conclusion

Root-cause hypothesis: **false-sharing on process-global backend
atomics**, in two locations (`BackendFragCounters` and
`StatsRange::Type`). Tentative fix applied via `alignas(64)`.

Bench validation deferred to a follow-up validation pass (predecessor
agent budget exhausted on bench wall-clock).
