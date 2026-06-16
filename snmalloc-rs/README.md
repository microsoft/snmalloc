# snmalloc-rs

[![snmalloc-rs CI](https://github.com/microsoft/snmalloc/actions/workflows/rust.yml/badge.svg)](https://github.com/microsoft/snmalloc/actions/workflows/rust.yml)

`snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as
a global allocator for rust. snmalloc is a research allocator. Its key design features are:

- Memory that is freed by the same thread that allocated it does not require any synchronising operations.
- Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel
  message passing scheme to return the memory to the original allocator, where it is recycled.
- The allocator uses large ranges of pages to reduce the amount of meta-data required.

Some old benchmark results are available in
the [`snmalloc` paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf). 

There are the following features defined in this crate:

- `debug`: Enable the `Debug` mode in `snmalloc`. This is also automatically enabled if Cargo's `DEBUG` environment variable is set to `true`.
- `native-cpu`: Optimize `snmalloc` for the native CPU of the host machine. (this is not a default behavior
  since `0.2.14`)
- `qemu`: Workaround `madvise` problem of QEMU environment
- `local_dynamic_tls`: Workaround cannot allocate memory in static tls block
- `build_cc`: Use of cc crate instead of cmake (cmake still default) as builder (more platform agnostic)
- `usecxx17`: Use C++17 standard
- `check`: Enable extra checks to improve security, see upstream [security docs](https://github.com/microsoft/snmalloc/tree/main/docs/security).
  Note that the `memcpy` protection is not enabled in Rust.
- `win8compat`: Improve compatibility for old Windows platforms (removing usages of `VirtualAlloc2` and other new APIs)
- `lto`: Links with InterProceduralOptimization/LinkTimeOptimization
- `notls`: Enables to be loaded dynamically, thus disable tls.
- `stats`: Enables allocation statistics.
- `libc-api`: Enables libc API backed by snmalloc.
- `usewait-on-address`: Enable `WaitOnAddress` support on Windows (enabled by default).
- `tracing`: Enable structured tracing/logging.
- `fuzzing`: Enable fuzzing support.
- `vendored-stl`: Use self-vendored STL.
- `check-loads`: Enable check loads feature.
- `pageid`: Enable page ID feature.
- `gwp-asan`: Enable GWP-ASan integration. Requires `SNMALLOC_GWP_ASAN_INCLUDE_PATH` and `SNMALLOC_GWP_ASAN_LIBRARY_PATH`.
- `profiling`: Enable the statistical heap profiler. Activates the C-side `SNMALLOC_PROFILE=ON` build and exposes the `HeapProfile` / `ProfilingSession` APIs documented below.
- `symbolicate`: Resolve raw frame addresses captured by the profiler into function/file/line via the [`backtrace`](https://crates.io/crates/backtrace) crate. Compose with `profiling`.

## Heap Profiling

The `profiling` Cargo feature enables a low-overhead statistical heap
profiler in the underlying snmalloc build. Each allocation has an
independent Poisson probability of being recorded with its call stack;
summing the per-sample weights gives an unbiased estimator of total
bytes allocated. The default sampling interval is 524 288 bytes
(512 KiB); see the upstream snmalloc README for guidance on adjusting
it for your workload. At the default rate the profiler adds **<1%
throughput overhead** (verified by `benches/profile_bench.rs`).

Enable in `Cargo.toml`:

```toml
[dependencies]
snmalloc-rs = { version = "0.7.4", features = ["profiling"] }
# Optional: resolve raw frame addresses to function/file/line.
# snmalloc-rs = { version = "0.7.4", features = ["profiling", "symbolicate"] }
```

### Quick start: snapshot + flamegraph

`SnMalloc::snapshot()` materialises an owned [`HeapProfile`] of every
currently-live sampled allocation. The profile can be written directly
in Brendan Gregg's folded-stack format, consumable by
[`inferno-flamegraph`](https://github.com/jonhoo/inferno) or
[Speedscope](https://www.speedscope.app/):

```rust
use snmalloc_rs::SnMalloc;
use std::fs::File;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

fn main() -> std::io::Result<()> {
    // 256 KiB mean sampling interval. Set to 0 to disable.
    ALLOC.set_sampling_rate(256 * 1024);

    // ... run your workload ...

    let profile = ALLOC.snapshot();
    let mut out = File::create("heap.folded")?;
    profile.write_flamegraph(&mut out)?;
    Ok(())
}
```

Then render to SVG:

```sh
inferno-flamegraph < heap.folded > heap.svg
```

### Streaming mode

For long-running services, `ProfilingSession::start` registers a
closure that receives a [`StreamSample`] for every sampled allocation
as it happens — no need to call `snapshot()` periodically. The session
is an RAII handle: dropping it unregisters the callback and tears down
all internal state.

```rust
use snmalloc_rs::{ProfilingSession, SnMalloc};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

let bytes_seen = Arc::new(AtomicU64::new(0));
let counter = Arc::clone(&bytes_seen);

let _session = ProfilingSession::start(move |sample| {
    counter.fetch_add(sample.weight(), Ordering::Relaxed);
})
.expect("no other session active");

// ... run workload ...
// Session is unregistered automatically when `_session` is dropped.
```

The closure must be `Fn + Send + Sync + 'static`; samples may be
dispatched on any thread that trips the sampler. Only one session can
be active per process at a time.

#### Realloc / Resize events

Each `StreamSample` carries an `EventKind` tag. `EventKind::Alloc` is
the original alloc-time broadcast; `EventKind::Resize` is emitted when
an in-place `realloc` updates the size of a previously-sampled
allocation, and carries the post-resize `requested_size` /
`allocated_size`. The original alloc-site stack and the sample's
Poisson weight are preserved across a Resize -- the sampler is not
re-rolled on resize. Out-of-place realloc (the slow path where snmalloc
actually allocates a new block and frees the old one) is described by
the existing Alloc + dealloc broadcasts; consumers that build a live
"bytes per call site" view can therefore treat Resize events as
in-place size churn on the same stack without double-counting.

```rust
use snmalloc_rs::streaming::EventKind;

let _session = ProfilingSession::start(|sample| {
    match sample.kind() {
        EventKind::Alloc => { /* a fresh sampled allocation */ }
        EventKind::Resize => { /* an in-place realloc grew/shrank it */ }
    }
});
```

### Runtime configuration via env vars

`SnMalloc::init_profiling_from_env()` reads `SNMALLOC_PROFILE_ENABLE`
and `SNMALLOC_PROFILE_RATE` from the process environment and applies
the resulting sampling rate without recompiling. This is the
recommended way to ship a binary that operators can flip into profiling
mode on demand:

```rust
use snmalloc_rs::SnMalloc;

#[global_allocator]
static ALLOC: SnMalloc = SnMalloc;

fn main() {
    // Honour SNMALLOC_PROFILE_ENABLE=1 / SNMALLOC_PROFILE_RATE=<bytes>.
    let _ = ALLOC.init_profiling_from_env();

    // ... your app ...
}
```

Resolution order:

1. If `SNMALLOC_PROFILE_RATE` is a parseable non-negative integer, it
   wins (including `0`, which explicitly disables).
2. Otherwise, a truthy `SNMALLOC_PROFILE_ENABLE` (`1` / `true` / `yes`,
   case-insensitive) enables sampling at the default 512 KiB rate.
3. Otherwise the call is a no-op — the sampling rate is unchanged.

Operators can then control profiling without rebuilding:

```sh
SNMALLOC_PROFILE_ENABLE=1 ./my-app                 # default 512 KiB
SNMALLOC_PROFILE_RATE=65536 ./my-app               # 64 KiB high-res
SNMALLOC_PROFILE_RATE=0 ./my-app                   # explicitly off
```

A typed `ProfileConfig` plus `SnMalloc::configure_profiling` is also
available when you want to apply a config programmatically rather than
via env vars.

### Typed configuration

```rust
use snmalloc_rs::{ProfileConfig, SnMalloc};

let cfg = ProfileConfig::with_sampling_rate(128 * 1024);
SnMalloc.configure_profiling(cfg);
```

### Google pprof output

`HeapProfile::write_pprof` emits the snapshot in Google's
[`pprof`](https://github.com/google/pprof) Profile protobuf format,
consumable by `go tool pprof`, Pyroscope, Polar Signals, Parca, and the
Datadog continuous profiler:

```rust
use snmalloc_rs::{SnMalloc, Weight};
use std::fs::File;

let profile = SnMalloc.snapshot();
let mut out = File::create("heap.pb")?;
profile.write_pprof(&mut out, Weight::Allocated)?;
# Ok::<(), std::io::Error>(())
```

Then inspect with the standard pprof tooling:

```sh
go tool pprof -http=:8080 heap.pb
```

Two sample-type axes are emitted: `("alloc_objects", "count")` and
`("alloc_space", "bytes")`. The `Weight::Allocated` projection
(default) reports bytes the allocator actually handed back including
sizeclass slack; `Weight::Requested` reports bytes the caller asked
for.

### Symbolicated output

With the additional `symbolicate` feature, the profiler resolves raw
frame addresses to function names, source files, and line numbers via
the `backtrace` crate. `write_flamegraph` then emits a symbolicated
folded-stack flamegraph **by default** -- the same call site as the
non-symbolicate build, no API change required:

```rust
# #[cfg(feature = "symbolicate")] {
use snmalloc_rs::SnMalloc;
use std::fs::File;

let profile = SnMalloc.snapshot();
let mut out = File::create("heap.folded")?;
profile.write_flamegraph(&mut out)?;
# }
# Ok::<(), std::io::Error>(())
```

Unresolved frames fall back to the same `0x` + 16-hex-digit rendering
used in the un-symbolicate build, so the renderer is total over
arbitrary frame addresses.

Callers who want the always-raw rendering -- e.g. to post-process the
addresses with an external symbolicator, or to keep golden output
stable across `symbolicate`-on / `symbolicate`-off builds -- can call
`write_flamegraph_raw` instead. Both methods are always available.

### Feature-off behaviour

When the `profiling` Cargo feature is **off**, every API listed above
remains callable but degrades gracefully:

- `SnMalloc::profiling_supported()` returns `false`.
- `SnMalloc::set_sampling_rate(...)` is a no-op; `sampling_rate()`
  reports `0`.
- `SnMalloc::snapshot()` returns an empty `HeapProfile`.
- `write_flamegraph` / `write_pprof` succeed and write a valid (empty)
  output.

This lets callers compile against the profiling API unconditionally
and turn it on or off via the Cargo feature alone.

## Build Configuration

The build script ensures architectural alignment between the Rust profile and the underlying `snmalloc` allocator:

### Environment Variables
The following environment variables are automatically detected and propagated:
- `DEBUG`: Synchronizes the `snmalloc` build type with the Cargo profile. If `true`, `snmalloc` is built in `Debug` mode.
- `OPT_LEVEL`: Propagated to the C++ compiler to ensure optimization parity between Rust and C++ components.

### Windows CRT Consistency
On Windows, the build script enforces static CRT linking (`/MT` or `/MTd`) across both `cc` and `cmake` builders. This prevents linker errors and ensures consistency when `snmalloc` is used as a global allocator.

**To get the crates compiled, you need to choose either `1mib` or `16mib` to determine the chunk configuration**

To use `snmalloc-rs` add it as a dependency:

```toml
# Cargo.toml
[dependencies]
snmalloc-rs = "0.7.4"
```

To set `SnMalloc` as the global allocator add this to your project:

```rust
#[global_allocator]
static ALLOC: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;
```

## For Android Cross-Compilation

- `ANDROID_NDK` must be provided as an environment variable
- `ANDROID_PLATFORM` can be passed as an optional environment variable
- `ANDROID_ABI` used by CMake is detected automatically
- feature `android-lld` can be used to set the linker of `snmalloc` to `lld`
- ~~feature `android-shared-std` can be used to set the STL library of `snmalloc` to `c++_shared` (it uses `c++_static` by
  default)~~ (`libstdc++` is no longer a dependency)

## Changelog

### 0.7.4

- Tracking upstream to match version 0.7.4.
- SnMalloc has been moved to upstream repository. Future releases will track upstream release directly.

### 0.3.8

- Tracking upstream to match version 0.7.1
- Recommended to upgrade from 0.3.7 to get an important bug fix.

### 0.3.7

- Tracking upstream to match version 0.7

### 0.3.4
- Tracking upstream to version 0.6.2.

### 0.3.3
- Tracking upstream to fix Linux PAL typo.

### 0.3.2

- Tracking upstream to enable old Linux variants.

### 0.3.1 

- Fixes `build_cc` feature (broken in 0.3.0 release).
- Fixes `native-cpu` feature (broken in 0.3.0 release).

### 0.3.0

- Release to support snmalloc 0.6.0.

### 0.3.0-beta.1

- Beta release to support snmalloc ~~2~~ 0.6.0
