# snmalloc-tools

Command-line tools that join external PMU output (Linux `perf`) with
snmalloc's in-tree allocation-site lookup and branch-hint inventory.

This crate is the Phase 10.4 automation surface for the workflow
documented in [`docs/profiling-pmu.md`](../docs/profiling-pmu.md). The
underlying primitives — `SnMalloc::lookup_alloc_site`,
`HeapProfile::top_sites`, and the `branch_hints.json` sidecar — landed
in Phases 10.1 and 10.2. This crate wraps them in a clap-derive CLI.

## Subcommands

```
snmalloc-tools profile-top --input <profile.pb> --n 10
    Print the top N allocation sites from a pprof Profile file.

snmalloc-tools pmu-join cache-misses --perf-script <file> [--top N] [--json]
    Parse `perf script` output; for samples with a data address, look
    up the allocating call site and rank by miss count.

snmalloc-tools pmu-join c2c --perf-c2c <file> [--top N] [--json]
    Parse `perf c2c report --stdio`; group HITM events by cache line
    and emit the owning allocation site per line.

snmalloc-tools branch-misses --perf-script <file> --hints <branch_hints.json> [--top N] [--json]
    Parse `perf script` output and cross-reference with the Phase
    10.2 branch-hint inventory.  High-miss-rate inverted hints are
    candidates for `LIKELY` <-> `UNLIKELY` swap.

snmalloc-tools rate-report --input <streaming-log.jsonl> [--top N] [--pretty]
    Stream-parse a snmalloc streaming event log (JSON Lines) and
    emit a per-site row: alloc/dealloc counts, peak live bytes,
    alloc-rate per second.  Output is CSV by default; `--pretty`
    emits a fixed-width table.  Stream-based — 6M-event logs use
    O(distinct sites) memory, not O(events).
```

All subcommands except `rate-report` accept `--json` for structured
output; the default is a plain-text table.  `rate-report` emits CSV
by default (the friendliest format for downstream awk/jq/spreadsheet
pipelines) and a fixed-width table under `--pretty`.

## Streaming event-log schema (`rate-report`)

`rate-report` consumes **JSON Lines** (UTF-8, one event object per
line).  The producer is typically an application using
[`snmalloc_rs::ProfilingSession`](../snmalloc-rs/src/streaming.rs)
that serialises each callback to a file.  Schema:

```jsonl
{"ts_ns": 1000000, "kind": "alloc", "site": "0x55a0c0001000", "size": 4096}
{"ts_ns": 1001000, "kind": "dealloc", "site": "0x55a0c0001000", "size": 4096}
```

Fields:

- `ts_ns` (u64, optional) — monotonic-clock timestamp in nanoseconds.
  Used to compute the alloc-rate denominator; when missing across all
  records the rate column is reported as `0.0`.
- `kind` (string, required) — one of `"alloc"`, `"dealloc"`,
  `"resize"`.  Unknown values are skipped (forward-compat).
- `site` (string, required) — the allocation site key.  Typically the
  leaf-frame address as `0x` + 16 hex digits, matching the
  `site_leaf` field emitted by the other subcommands.
- `size` (u64, optional) — bytes attributable to this event.

Malformed lines are skipped silently — the reader is resilient to
truncated tails and the occasional blank line.  See
`tests/fixtures/streaming_log_sample.jsonl` for a worked example.

## Snapshot vs streaming

`profile-top` walks a `HeapProfile::snapshot()` (currently-live
sampled allocations) and is biased toward long-lived state;
`rate-report` walks a streaming log and captures transient churn.
See the "When to use snapshot vs streaming" section in
[`../snmalloc-rs/README.md`](../snmalloc-rs/README.md) for a fuller
treatment of the tradeoff.

## Live-process limitation (important)

`SnMalloc::lookup_alloc_site` (Phase 10.1) only resolves addresses
that were sampled in the **current** process — it queries the
per-process in-memory `SampledList`, not a serialised snapshot. This
means the `pmu-join cache-misses` and `pmu-join c2c` subcommands are
only useful in two scenarios:

1. **In-process joiner.** The workload itself calls into
   `snmalloc-tools` (as a library — see `src/lib.rs`) at the end of
   the run, before the live allocations are freed. The integration
   test `cache_miss_joiner_resolves_in_process_allocation` shows the
   shape: hold a live allocation, then feed its address through the
   joiner.

2. **Replay with the same allocations.** A second process can re-run
   the same allocation pattern, sampled at a high enough rate that
   the addresses re-converge with the original recording. This is
   best-effort; for production attribution, prefer (1).

Out-of-process, post-hoc runs against a pre-recorded perf file with a
*different* process will see every sample as "unattributed". The
`pmu-join c2c` subcommand specifically keeps unattributed lines in
its output (with `site_leaf = "<unattributed>"`) so the operator can
still see the HITM count.

The `branch-misses` subcommand has **no** live-process restriction;
the branch-hint inventory is a static sidecar.

## Fixtures

`tests/fixtures/` ships minimal hand-crafted samples for each parser:

- `perf_script_sample.txt` — three samples (branch-miss IP-only,
  cache-miss IP-only, mem-load with data address).
- `perf_c2c_sample.txt` — two contended cache lines with detail rows.
- `branch_hints_sample.json` — three hint sites matching the schema
  in `scripts/dump_branch_hints.py`.
- `streaming_log_sample.jsonl` — eight events across two sites,
  exercising alloc, dealloc, resize, and the peak-then-drop pattern
  that `rate-report` is built to surface.

The integration tests in `tests/integration.rs` exercise each
parser/joiner against these fixtures.

## Cross-references

- Phase 10.1 — `src/snmalloc/profile/addr_lookup.h` and
  `snmalloc-rs/src/profile.rs::SnMalloc::lookup_alloc_site`
- Phase 10.2 — `scripts/dump_branch_hints.py` and the
  `branch_hints_inventory` CMake target
- Phase 10.3 — `docs/profiling-pmu.md`
