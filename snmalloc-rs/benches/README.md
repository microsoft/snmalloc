# `snmalloc-rs` benchmarks

This directory contains the Criterion-driven benchmark suite used to
measure the per-allocation latency overhead of the heap-profiling
instrumentation (`SNMALLOC_PROFILE` on the C++ side; the `profiling`
Cargo feature on the Rust side).

## Running

```bash
# Baseline -- profile-off (single variant per group).
cargo bench --bench profile_bench

# Profiling-on -- three variants per group:
#   profile-off          (always-off branch, control)
#   profile-on-inactive  (countdown active, sample rate = usize::MAX)
#   profile-on-active    (countdown active, sample rate = 512 KiB default)
cargo bench --bench profile_bench --features profiling
```

A full sweep takes ~2-3 minutes on a recent laptop.  Criterion writes
detailed reports (per-group HTML pages, JSON estimates) under
`target/criterion/`; the bench binary also prints a one-paragraph
summary to stderr at the end of the run pointing at the key files.

## What to look at

The number to focus on is **`ratio_idle`**, defined per benchmark
group as:

```
ratio_idle = mean(profile-on-inactive) / mean(profile-off)
```

That is the latency cost paid by a binary that compiles in the
profiling support but never enables sampling -- i.e. the cost an end
user sees when they build with `--features profiling` "just in case"
and leave it dormant.  Phase 7.1 cache-line-aligned the sample
countdown specifically to push this number below 5%, so a regression
above ~1.05 in any of the three groups is worth investigating.

The `profile-on-active` numbers, by contrast, measure the cost of
actually taking the slow path.  They are larger and that's expected;
the headline 512 KiB rate hits the sampler roughly once per ~16 K
small allocations, and the per-sample stack capture dominates that
column.  Compare against the previous baseline rather than against
`profile-off`.

## Absolute numbers

Absolute ns/alloc numbers depend heavily on the host, the C++ build
flags (`debug` vs release, `check`, etc.) and the OS allocator path
behind the global allocator.  This suite is designed for **relative**
comparisons (variant-vs-variant within a single run, or run-vs-run on
the same machine).  Don't compare raw numbers across machines; do
compare ratios.
