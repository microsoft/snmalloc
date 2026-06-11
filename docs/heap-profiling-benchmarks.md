# Heap Profiling Benchmarks

This document records the measured per-allocation latency overhead of the
`profiling` Cargo feature in `snmalloc-rs`, as produced by the Criterion
bench suite at [`snmalloc-rs/benches/profile_bench.rs`](../snmalloc-rs/benches/profile_bench.rs)
(see also that file's module-level doc-comment and the companion
[benches README](../snmalloc-rs/benches/README.md)).

The point of this page is to replace the previously-unverified design
target ("<1% overhead at default sampling rate") with **measurement**.
The numbers below are produced on a single machine and are intended for
relative comparison (variant-vs-variant within a run) rather than
absolute cross-host comparison.

## Machine configuration

| Item              | Value                                                                                 |
|-------------------|---------------------------------------------------------------------------------------|
| Host kernel       | `Darwin 25.3.0` (xnu-12377.91.3, RELEASE_ARM64_T6041)                                 |
| OS                | macOS 26.3.1 (build 25D2128)                                                          |
| Architecture      | `arm64`                                                                               |
| CPU               | Apple M4 Pro                                                                          |
| Logical cores     | 12                                                                                    |
| RAM               | 24 GiB                                                                                |
| Toolchain         | `rustc 1.95.0 (59807616e 2026-04-14)`                                                 |
| Allocator under test | `snmalloc` via `snmalloc-rs` (release profile, `--features profiling`)             |
| Bench harness     | `criterion` 0.5 (`default-features = false`), 3s warm-up + 5s measure, 50 samples    |
| Batch per sample  | 64 alloc + 64 dealloc per inner iteration                                             |

The bench binary itself does **not** install `SnMalloc` as the global
allocator; allocations go through `std::alloc::{alloc, dealloc}` on the
host's default allocator. The numbers therefore measure the **relative**
cost of the in-process profiling instrumentation (countdown decrement on
the snmalloc-side FFI getter/setter and the conditional sampling slow
path), not absolute snmalloc throughput. This is consistent with the
bench's stated design (see the comment on `alloc_batch` in
`profile_bench.rs`).

## Raw results

All numbers are **mean ns / allocation-batch** (one criterion iteration =
64 allocs + 64 deallocs), with the 95% confidence interval reported by
criterion's bootstrap. Source JSON: `target/criterion/*/new/estimates.json`.

### `small_allocs` (32-byte allocations)

| Variant                | Mean (ns) | 95% CI (ns)            | Median (ns) | Std dev (ns) |
|------------------------|----------:|------------------------|------------:|-------------:|
| profile-off            |    791.36 | [784.65, 801.22]       |      786.34 |        31.00 |
| profile-on-inactive    |    787.77 | [779.64, 798.11]       |      782.11 |        33.85 |
| profile-on-active      |    783.98 | [779.54, 788.62]       |      778.56 |        16.56 |

### `medium_allocs` (4 KiB allocations)

| Variant                | Mean (ns) | 95% CI (ns)            | Median (ns) | Std dev (ns) |
|------------------------|----------:|------------------------|------------:|-------------:|
| profile-off            |   3050.45 | [3041.13, 3060.71]     |     3043.61 |        35.73 |
| profile-on-inactive    |   3200.72 | [3125.80, 3302.24]     |     3087.60 |       324.14 |
| profile-on-active      |   3179.58 | [3088.19, 3339.77]     |     3086.37 |       515.40 |

### `mixed` (LCG-driven sizes in `[16, 16384)`)

| Variant                | Mean (ns) | 95% CI (ns)            | Median (ns) | Std dev (ns) |
|------------------------|----------:|------------------------|------------:|-------------:|
| profile-off            |   1378.37 | [1372.18, 1384.69]     |     1378.42 |        22.78 |
| profile-on-inactive    |   1416.47 | [1398.42, 1446.78]     |     1401.18 |        96.77 |
| profile-on-active      |   1431.77 | [1391.47, 1494.51]     |     1389.31 |       198.02 |

## Ratios

`ratio_idle = mean(profile-on-inactive) / mean(profile-off)` — the cost
paid by a binary that compiles in profiling support but never enables
sampling (the "always-on instrumentation" cost).

`ratio_active = mean(profile-on-active) / mean(profile-off)` — the cost
paid at the documented default sampling rate (524 288 bytes ~ 512 KiB).

| Group           | ratio_idle | ratio_active |
|-----------------|-----------:|-------------:|
| small_allocs    |     0.9955 |       0.9907 |
| medium_allocs   |     1.0493 |       1.0423 |
| mixed           |     1.0276 |       1.0387 |
| **average**     | **1.0241** |   **1.0239** |
| **max**         | **1.0493** |   **1.0423** |

`small_allocs` actually came in *below* 1.0 — the `profile-on-inactive`
and `profile-on-active` means are slightly under `profile-off`. This is
within the noise band: the confidence intervals overlap heavily (all
three intervals on `small_allocs` straddle ~785 ns), and the variant
medians are within ~8 ns of each other. We do **not** interpret this as
"profiling makes allocations faster"; it is consistent with the
instrumentation being effectively free on the 32-byte fast path on this
host. The cache-line alignment of the sample countdown from Phase 7.1 is
doing its job here.

## Comparison vs README claim

Both `README.md` and `snmalloc-rs/README.md` currently advertise
**"<1% throughput overhead"** at the default sampling rate, citing this
bench suite. The measurement on this host does **not fully support** that
claim:

- On `small_allocs` the claim holds (sub-1% in both idle and active,
  in fact slightly *negative* and within noise of zero — see above).
- On `medium_allocs` and `mixed` the active configuration adds
  **~4%** (3.87% on mixed, 4.23% on medium), and the idle
  configuration adds **~2.8% to ~4.9%**. Maximum observed ratio is
  **1.0493** on `medium_allocs/profile-on-inactive`.
- Across all groups, the *average* overhead is **~2.4%** in both the
  idle and active configurations.

So the data supports the looser statement "well under 5% overhead at the
default sampling rate, on every group measured" — which is the
acceptance threshold the benches README actually checks against
(`ratio_idle <= 1.05`). It does **not** support the headline "<1%"
phrasing for the `medium_allocs` and `mixed` workloads.

There are three plausible explanations for the gap, none of which we
attempt to disambiguate in this results-publishing PR:

1. The 4 KiB and mixed-size paths in the host allocator (recall this
   bench runs through `std::alloc` on the system allocator, not on
   snmalloc-as-global) have more variance than the 32-byte path, so the
   confidence intervals on `profile-on-*` for those groups are
   ~10x wider than for `small_allocs`. The point estimate moves around
   more, and the bench reports the mean. This is visible in the std-dev
   column above (324 ns and 515 ns on medium, vs 31 ns on small).
2. The "<1%" target was set against an earlier baseline (Phase 7.1's
   cache-line alignment work) on a different host. On Apple M4 Pro the
   per-allocation cost on the 4 KiB path is dominated by zeroing and
   page-faults, which interact with the sampling countdown in
   ways that have not been re-measured since the C++ profiling hook
   landed.
3. The bench harness does not pin to a single performance core, does
   not disable Turbo Boost, and runs on a laptop where macOS may schedule
   the bench thread onto an efficiency core under thermal load. The
   `profile-on-active` runs come last and may see slightly different
   thermal state than `profile-off`.

## Status

The README "<1% overhead at default sampling rate" claim is the design
target; this measurement does not yet support it on `medium_allocs` and
`mixed` workloads. The gap is being driven to closure in
[ClickUp ticket 86aj0hfmc](https://app.clickup.com/t/86aj0hfmc).
Investigation targets in priority order: cache-line alignment of the
Sampler hot state, the Phase 3.3 alloc-hook fast path, the dealloc
null-check predictor behavior, and lazy-provider first-touch costs on
medium/mixed slabs. Re-run this document after that work lands.

Reproduction caveats worth noting before the perf investigation begins:

- Re-run on a Linux host with `taskset` pinning and `cpufreq` set to
  `performance` to remove the macOS scheduler variance.
- Raise `sample_size` on `medium_allocs` and `mixed` so the confidence
  intervals tighten enough to distinguish a real ~3% overhead from
  noise (the std-dev column above shows medium at 324–515 ns vs small
  at 31 ns).

## Reproducing

```bash
cd snmalloc-rs
cargo bench --features profiling
# Numbers land in target/criterion/<group>/<variant>/new/estimates.json
```

A full sweep is three groups x three variants x (3s warm-up + 5s
measure) plus criterion bootstrap overhead — roughly 80-90 seconds of
wall-clock on the host above. No group hit the 20-minute time budget;
no group was skipped.
