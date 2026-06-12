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
```

All subcommands accept `--json` for structured output; the default is
a plain-text table.

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

The integration tests in `tests/integration.rs` exercise each
parser/joiner against these fixtures.

## Cross-references

- Phase 10.1 — `src/snmalloc/profile/addr_lookup.h` and
  `snmalloc-rs/src/profile.rs::SnMalloc::lookup_alloc_site`
- Phase 10.2 — `scripts/dump_branch_hints.py` and the
  `branch_hints_inventory` CMake target
- Phase 10.3 — `docs/profiling-pmu.md`
