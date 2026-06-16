# Bazel integration cookbook

This page collects the patterns we use to run snmalloc-rs heap-profile
collection from inside a Bazel-built binary or `rust_test`. It assumes
familiarity with the `profiling` Cargo feature (see the main
`snmalloc-rs/README.md` for the API surface).

## Profile-output path resolution

`snmalloc_rs::profile::default_output_path()` (gated on the `profiling`
feature) returns a `PathBuf` chosen by the following precedence chain.
First match wins:

1. **`SNMALLOC_PROFILE_OUT`** — explicit override. Whatever the
   operator / CI script puts here is used verbatim. This is the escape
   hatch you want to wire into a `--test_env=` flag (Bazel test) or a
   `--action_env=` flag (Bazel binary) so you can redirect output
   without recompiling the workload.
2. **`$TEST_UNDECLARED_OUTPUTS_DIR/heap.folded`** — Bazel's
   per-test scratch directory. When a `rust_test` runs under
   `bazel test`, Bazel sets `TEST_UNDECLARED_OUTPUTS_DIR` to a
   per-action directory and automatically picks up anything written
   there as a declared test artefact. The file ends up in the
   `outputs.zip` attached to the test result and is visible in the
   Build Event Service (BES) / Remote Build Execution (RBE) UI without
   any extra wiring.
3. **`$TMPDIR/heap_{pid}.folded`** — final fallback for plain
   `cargo run` / `cargo test` / interactive `bazel run`. The PID is
   appended so two concurrent processes don't clobber each other's
   output.

The three rungs are intentionally ordered from "most explicit" to
"safest default". The `SNMALLOC_PROFILE_OUT` override is the only one
that respects the literal path you set — both of the other rungs
synthesize a filename for you. Callers writing pprof or another
format should `with_extension(...)` the returned path; the default
suffix is `.folded` because that's the most broadly consumable format
emitted by `HeapProfile::write_flamegraph`.

## BES upload size considerations

Bazel's Build Event Service uploads test artefacts back to the
result-store on every `bazel test` invocation. The default per-file
upload size cap is around **10 MiB**; profiles larger than that will
either be truncated or rejected depending on the BES backend. A few
practical implications:

- A 512 KiB sampling rate (the snmalloc default) keeps a folded-stack
  profile well under the cap for workloads up to a few minutes of
  steady-state allocation. If your workload runs longer, raise the
  sampling rate (set `SNMALLOC_PROFILE_RATE=2097152` for 2 MiB, etc.)
  to keep the output bounded.
- For very long-running test workloads, rotate the output: take a
  snapshot every N seconds, write it to a numbered file under
  `$TEST_UNDECLARED_OUTPUTS_DIR/heap_{N}.folded`, and let downstream
  tooling stitch them. The `default_output_path` helper only resolves
  a single path; rotation is a one-line `with_file_name()` away.
- Gzipped pprof (`HeapProfile::write_pprof_gz`) typically shrinks
  output 5–10x versus the folded form. If you're already collecting
  pprof, prefer the gzipped variant for the BES round-trip.

## Example `BUILD.bazel` snippet

The minimal opt-in pattern for a `rust_test` that wants to dump a
heap profile on exit:

```python
load("@rules_rust//rust:defs.bzl", "rust_test")

rust_test(
    name = "my_heap_profile_test",
    srcs = ["tests/my_heap_profile_test.rs"],
    edition = "2021",
    deps = [
        "//snmalloc-rs:snmalloc_rs",
    ],
    # Opt the test into snmalloc's heap profiler at a 256 KiB
    # sampling rate.  `SNMALLOC_PROFILE_OUT` is left unset so the
    # path-resolution chain falls through to TEST_UNDECLARED_OUTPUTS_DIR,
    # which Bazel auto-uploads as a test artefact.
    env = {
        "SNMALLOC_PROFILE_ENABLE": "1",
        "SNMALLOC_PROFILE_RATE": "262144",
    },
)
```

If you want to override the path explicitly — e.g. to dump to a known
location for a downstream `genrule` to consume — extend `env` with
`SNMALLOC_PROFILE_OUT`:

```python
    env = {
        "SNMALLOC_PROFILE_ENABLE": "1",
        "SNMALLOC_PROFILE_RATE": "262144",
        "SNMALLOC_PROFILE_OUT": "/tmp/explicit_heap.folded",
    },
```

For a `rust_binary` invoked via `bazel run`, swap `env` for the same
keys on a wrapper `sh_binary` or pass `--action_env=...` on the
command line. The resolution chain in `default_output_path()` is
identical regardless of the host rule kind — the helper only inspects
the process environment at call time.

## Choosing a profiling variant

The fork exposes three `rust_library` targets for the profiling
matrix. Pick the one matching how the consumer binary will read the
output:

| Target | Cargo features | Output frames | When to use |
| --- | --- | --- | --- |
| `:snmalloc_rs` | _(none)_ | n/a (no profiler) | Default. The Cargo `profiling` feature is off and the C-side `SNMALLOC_PROFILE` is off too. |
| `:snmalloc_rs_profiling` | `profiling` | 16-hex-digit raw addresses | Profiler on, but the pprof / folded output carries `0x...` frames. Operators symbolize externally with `atos` / `addr2line` / `llvm-symbolizer`. Lightest dep footprint. |
| `:snmalloc_rs_profiling_symbolicated` | `profiling`, `symbolicate` | Resolved `function (file:line)` frames | Profiler on **and** frames are resolved in-process via the `backtrace` crate at dump time. Drop the pprof straight into Grafana Pyroscope / Polar Signals / `go tool pprof -http=:8080 -` and the function names show up. |

The `:snmalloc_rs_profile_compat` target also exists as an escape
hatch — it binds the profile-enabled C archive but leaves the Cargo
`profiling` feature off (and therefore does not depend on flate2).
That's useful for downstream Bazel modules that cannot resolve
`@crates//:flate2` from this fork's `crate_universe` extension.

### Switching a downstream binary to symbolicated output

Change a single `deps` entry in the consumer `BUILD.bazel`:

```python
# Before — operators have to atos every frame by hand:
rust_binary(
    name = "konfig_bin_heapprof",
    srcs = [...],
    deps = [
        "@snmalloc//snmalloc-rs:snmalloc_rs_profiling",
        # ...
    ],
)

# After — frames are resolved at dump time, pprof viewers show
# function names directly:
rust_binary(
    name = "konfig_bin_heapprof",
    srcs = [...],
    deps = [
        "@snmalloc//snmalloc-rs:snmalloc_rs_profiling_symbolicated",
        # ...
    ],
)
```

No API change is required — `HeapProfile::write_flamegraph` and
`HeapProfile::write_pprof_gz` are the same call sites as in the
un-symbolicate build. The renderer detects the feature at compile
time and emits resolved frames automatically; unresolved frames
(kernel, JIT, stripped code) fall back to the same 16-hex-digit
form as the non-symbolicate variant. See the
`snmalloc-rs/README.md` "Symbolicated output" section for the
matching Cargo-side recipe and the runtime caveats.

### Cost

`backtrace` pulls `addr2line` + `gimli` + `object` transitively
(~500 kB live set, parses the binary's debug info on first use). If
the consumer binary already links those crates for any other reason
(panic backtraces, `tracing-error`, etc.) the incremental cost is
near zero — which is the common case for production Rust binaries.
If you measure the cost and it's unwelcome, stay on
`:snmalloc_rs_profiling` and symbolize the raw pprof externally.
