# PMU profiling with snmalloc

This document describes the supported workflow for attributing CPU
performance-monitoring-unit (PMU) events — cache misses, false sharing,
and branch mispredictions — back to the snmalloc call sites and
allocations that caused them. snmalloc itself does **not** sample PMU
counters: that work is delegated to the OS-provided profilers
(`perf` on Linux, Instruments on macOS). snmalloc's contribution is to
expose enough metadata about allocations and hint sites that the raw
samples can be **joined** with allocator state.

> **Forward references.** This document references three companion
> deliverables. Items marked *(10.1)* depend on the Phase 10.1 in-tree
> allocation-site lookup API, items marked *(10.2)* depend on the
> Phase 10.2 branch-hint inventory sidecar, and items marked *(10.4)*
> depend on the Phase 10.4 `snmalloc-tools` CLI that automates the
> joins shown here. Each is available once the corresponding phase
> lands; the manual command sequences below work today against the
> primitives that already exist.
>
> Phase 10.4 is now merged: the joins below are automated via the
> `snmalloc-tools` subcommands listed in the table (`profile-top`,
> `pmu-join cache-misses`, `pmu-join c2c`, `branch-misses`).  See
> `snmalloc-tools/README.md` for the live-process limitation that
> applies to the cache-miss / c2c joiners.

## Overview

| CPU microarch gap | snmalloc in-tree API | External tool | `snmalloc-tools` subcommand |
| ----------------- | -------------------- | ------------- | --------------------------- |
| Allocation hot-spots | `HeapProfile::top_sites()` *(10.1)* | none — built in | `snmalloc-tools profile-top` *(10.4)* |
| Cache-miss attribution (Linux) | `snmalloc::lookup_alloc_site(addr)` *(10.1)* | `perf record -e cache-misses` | `snmalloc-tools pmu-join cache-misses` *(10.4)* |
| False sharing (Linux) | `snmalloc::lookup_alloc_site(addr)` *(10.1)* | `perf c2c record` | `snmalloc-tools pmu-join c2c` *(10.4)* |
| Cache-miss attribution (macOS) | `snmalloc::lookup_alloc_site(addr)` *(10.1)* | Instruments (System Trace → Counters) | `snmalloc-tools pmu-join instruments` *(10.4)* |
| Branch-hint miss rates | `branch_hints.json` *(10.2)* | `perf record -e branch-misses` | `snmalloc-tools branch-misses` *(10.4)* |

The remainder of this document is one recipe per row.

## 1. Allocation hot-spots

This is the only one of the four gaps that snmalloc answers entirely
in-tree: the statistical heap profiler shipped in Phase 7 already
records per-allocation call stacks (see the
[Heap Profiling](../README.md#heap-profiling) section of the project
README and `docs/heap-profiling-benchmarks.md`). Phase 10.1 adds a
`top_sites()` convenience method on top of the existing
`HeapProfile` snapshot type that bucket-sorts samples by their leaf
frame and returns the heaviest call sites by bytes requested.

> Available once Phase 10.1 lands.

### Rust example *(10.1)*

```rust
use snmalloc_rs::SnMalloc;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

fn main() {
    SnMalloc::init_profiling_from_env();

    // ... run the workload ...

    let snapshot = SnMalloc::heap_profile().expect("profiling enabled");
    for site in snapshot.top_sites(10) {
        println!(
            "{:>10} bytes  {:>6} samples  {}",
            site.bytes_requested,
            site.sample_count,
            site.leaf_symbol.as_deref().unwrap_or("<unresolved>"),
        );
    }
}
```

### Example output

```
   8.45 MiB     132 samples  my_app::parser::Token::clone
   4.21 MiB      67 samples  my_app::graph::Node::new
   2.10 MiB      33 samples  alloc::vec::Vec::reserve
   ...
```

The numeric columns are unbiased Poisson estimators of total bytes
requested through that leaf, scaled across the entire snapshot.

**Automated via `snmalloc-tools profile-top` — see Phase 10.4.**

## 2. Cache-miss attribution (Linux)

`perf` samples the hardware cache-miss counter and records the
instruction pointer + call stack at each sample. snmalloc's
contribution is `lookup_alloc_site(addr)` *(10.1)*, which takes a data
address (typically the one that missed the cache, recovered from the
sample's PEBS / IBS load-latency record) and returns the call site
that allocated the chunk containing it.

### Capture

```bash
# Pick the target PID. -p replaces -a if you only want this process.
perf record \
    -e cache-misses \
    --call-graph dwarf \
    -p "$PID" \
    -- sleep 30

perf script > samples.txt
```

`perf script` emits one block per sample: an event header, the data
address (if the PMU event supports it — `mem_load_*` events do, raw
`cache-misses` may not), the instruction pointer, and the stack.

### Join with snmalloc *(10.1)*

For each sample whose data address falls within an snmalloc-managed
region, call `snmalloc::lookup_alloc_site(addr)` from a small C++
harness (or, via the Rust crate, the safe wrapper exposed in
Phase 10.1) to recover the allocation call stack. Pair the
instruction-pointer stack (the *consumer* — who was reading the
memory when it missed) with the allocation-site stack (the *producer*
— who allocated the missing line) to localize the layout problem.

For raw `cache-misses` samples that don't carry a data address,
manually grep `samples.txt` for IPs known to live in your hot path,
then look up the *first argument* (the pointer being touched) from
the surrounding stack. The Phase 10.4 joiner automates the data-addr
case and falls back to IP-only attribution otherwise.

**Automated via `snmalloc-tools pmu-join cache-misses` — see Phase 10.4.**

## 3. False-sharing detection (Linux)

`perf c2c` ("cache-to-cache") sniffs HITM events — loads that were
served from a *modified* line in another core's cache — and groups
them by cache line. Lines with high HITM counts are the false-sharing
suspects.

### Capture

```bash
perf c2c record -a -- ./my-app

# --stdio dumps the full report; the curses TUI is also useful interactively.
perf c2c report --stdio > c2c.txt
```

The report's "Shared Data Cache Line Table" lists each contended line
with its physical / virtual address, the offsets within the line that
were accessed, and the producing / consuming code locations.

### Join with snmalloc *(10.1)*

For each contended line, pass its virtual address to
`snmalloc::lookup_alloc_site(addr)`. Because `lookup_alloc_site`
returns the allocation that owns the *chunk* containing the address,
even sub-cache-line offsets resolve back to the allocation site that
placed the two contended fields on the same line. Common results:

- Two distinct `struct` fields land on the same line → reorder or
  pad the struct.
- Two array elements from a shared-mutable container collide → align
  the allocation to a cache line.

**Automated via `snmalloc-tools pmu-join c2c` — see Phase 10.4.**

## 4. Cache-miss attribution (macOS)

Apple does not expose a `perf`-equivalent public API. The kperf
framework that drives the per-CPU counters is a private SPI and is
not callable from third-party processes without entitlements. The
supported, no-root path is **Instruments**.

### Capture

1. Launch **Instruments** (ships with Xcode).
2. Choose the **System Trace** template.
3. Add the **Counters** instrument and configure it to sample one of
   the cache-miss-related events (`L1D_CACHE_MISS_LD`, `L2_TLB_MISS`,
   etc. — the exact names depend on the CPU family).
4. Attach to your process and record.
5. **File → Export…** the trace as XML / `.trace` package.

### Join with snmalloc *(10.1, 10.4)*

Feed the exported trace to `snmalloc-tools pmu-join instruments`
*(10.4)*. The tool walks the Counters samples, extracts data
addresses (when present) and IP stacks, and joins them against
`lookup_alloc_site` exactly as on Linux.

### Limitations

- kperf is a private SPI; per-process cache-miss sampling without
  root is limited compared to `perf`. Some events are only visible
  system-wide.
- Data-address attribution is not exposed for all events on all
  Apple Silicon generations. Where unavailable, the join degrades to
  IP-only attribution (consumer side only — you still see *who* was
  missing, just not *which allocation* they were missing on).
- Instruments traces are large; prefer short capture windows
  (10–30s) over long recordings.

**Automated via `snmalloc-tools pmu-join instruments` — see Phase 10.4.**

## 5. Branch-hint miss rates

snmalloc's hot path is annotated with `SNMALLOC_LIKELY` /
`SNMALLOC_UNLIKELY` macros. A stale hint — one whose actual
probability has drifted from the source-code assumption — costs a
mispredicted branch on every hot-path invocation. Phase 10.2 emits a
`branch_hints.json` sidecar at build time that enumerates every hint
site with its source location and predicted direction; joining that
inventory with `perf record -e branch-misses` reveals stale hints.

### Capture

```bash
perf record -e branch-misses -- ./my-app
perf report --stdio --no-children | head -100 > branch-misses.txt
```

Restrict the report to symbols inside snmalloc to keep the noise down:

```bash
perf report --stdio --no-children --symbol-filter='snmalloc' \
    > snmalloc-branch-misses.txt
```

### Join with `branch_hints.json` *(10.2)*

The sidecar's schema is one entry per hint:

```json
{
  "file": "src/snmalloc/mem/freelist.h",
  "line": 412,
  "direction": "LIKELY",
  "symbol": "snmalloc::FreeListBuilder<...>::add"
}
```

For each high-sample-count entry in `branch-misses.txt`, look up its
source location (via `addr2line` against the binary's DWARF) and
match against `branch_hints.json`. A hint site whose miss rate
exceeds ~5% is a candidate for inversion (swap `LIKELY` ↔
`UNLIKELY`) or removal.

**Automated via `snmalloc-tools branch-misses` — see Phase 10.4.**

## What snmalloc does NOT do

By design, snmalloc keeps its allocator hot path free of PMU
sampling code. Specifically:

- **No built-in PMU sampling in the allocator binary.** snmalloc does
  not call `perf_event_open`, does not link against libpfm, and does
  not arm any hardware counters at runtime.
- **No kperf / private-SPI calls on macOS.** snmalloc never touches
  kperf. Cache-miss data on macOS must come from Instruments.
- **No ETW counters on Windows.** snmalloc does not register any ETW
  providers for PMU events.
- **No on-line cache-miss attribution.** The allocator does not learn
  about cache misses at runtime; it has no callback path from the CPU
  to the allocator. Attribution is offline, after `perf` / Instruments
  has finished recording.

These are deliberate non-goals. The OS-provided profilers do the
sampling work much better than an in-process sampler could, and
keeping the allocator hot path free of PMU plumbing preserves
snmalloc's "two-branch fast path" property. snmalloc's job is to
expose *enough metadata* (allocation sites, branch-hint inventory)
that the external samples can be attributed back to allocator
behavior; the sampling itself stays outside.
