# snmalloc

snmalloc is a high-performance allocator. 
snmalloc can be used directly in a project as a header-only C++ library, 
it can be `LD_PRELOAD`ed on Elf platforms (e.g. Linux, BSD),
and there is a [crate](https://crates.io/crates/snmalloc-rs) to use it from Rust.

Its key design features are:

* Memory that is freed by the same thread that allocated it does not require any
  synchronising operations.
* Freeing memory in a different thread to initially allocated it, does not take
  any locks and instead uses a novel message passing scheme to return the
  memory to the original allocator, where it is recycled.  This enables 1000s of remote 
  deallocations to be performed with only a single atomic operation enabling great
  scaling with core count. 
* The allocator uses large ranges of pages to reduce the amount of meta-data
  required.
* The fast paths are highly optimised with just two branches on the fast path 
  for malloc (On Linux compiled with Clang).
* The platform dependencies are abstracted away to enable porting to other platforms. 

snmalloc's design is particular well suited to the following two difficult 
scenarios that can be problematic for other allocators:

  * Allocations on one thread are freed by a different thread
  * Deallocations occur in large batches

Both of these can cause massive reductions in performance of other allocators, but 
do not for snmalloc.

The implementation of snmalloc has evolved significantly since the [initial paper](snmalloc.pdf).
The mechanism for returning memory to remote threads has remained, but most of the meta-data layout has changed.
We recommend you read [docs/security](./docs/security/README.md) to find out about the current design, and 
if you want to dive into the code [docs/AddressSpace.md](./docs/AddressSpace.md) provides a good overview of the allocation and deallocation paths.

[![snmalloc CI](https://github.com/microsoft/snmalloc/actions/workflows/main.yml/badge.svg)](https://github.com/microsoft/snmalloc/actions/workflows/main.yml)

# Hardening

There is a hardened version of snmalloc, it contains

*  Randomisation of the allocations' relative locations,
*  Most meta-data is stored separately from allocations, and is protected with guard pages,
*  All in-band meta-data is protected with a novel encoding that can detect corruption, and
*  Provides a `memcpy` that automatically checks the bounds relative to the underlying malloc.

A more comprehensive write up is in [docs/security](./docs/security/README.md).

# Further documentation

 - [Instructions for building snmalloc](docs/BUILDING.md)
 - [Instructions for porting snmalloc](docs/PORTING.md)

## Heap Profiling

snmalloc ships with an opt-in, low-overhead **statistical heap profiler**.
When enabled at build time, the allocator captures a Poisson-distributed
sample of every allocation with its call stack, suitable for offline
analysis with the same tooling (flamegraphs, pprof) commonly used for
CPU profiles.

### Enabling at build time

The profiler is gated behind a single CMake option, off by default:

```sh
cmake -B build -DSNMALLOC_PROFILE=ON
cmake --build build
```

With `SNMALLOC_PROFILE=OFF` (the default) every profiling code path is
compiled out — the sampler countdown, the per-allocation branch, and
the FFI export bodies all degrade to empty stubs. There is **no**
runtime cost for builds that do not opt in.

### What it samples

Each allocation has an independent probability of being recorded,
governed by a single tunable: the *mean sampling interval*, expressed
in bytes. The default is **524 288 bytes (512 KiB)**, meaning the
sampler captures roughly one allocation per 512 KiB of total request
volume. Per-sample weights are unbiased Poisson estimators, so summing
`weight` across the snapshot yields an unbiased estimate of total bytes
requested (or, scaled by `allocated_size / requested_size`, of total
bytes the allocator actually handed back).

The sampling rate can be adjusted at runtime: lowering it (e.g. to
64 KiB) gives higher resolution and ~1.5% throughput overhead;
raising it (e.g. to 1 MiB) reduces overhead further at the cost of
fidelity. See `docs/profile-weight.md` for guidance on choosing a rate
for your workload.

### C ABI for embedding

The C++ build exposes a small set of `extern "C"` symbols for
embedders that want to drive the profiler from a non-Rust host:

| Symbol | Purpose |
| ------ | ------- |
| `sn_rust_profile_supported` | Returns `true` iff built with `SNMALLOC_PROFILE=ON`. |
| `sn_rust_profile_set_sampling_rate` | Set the mean sampling interval in bytes. `0` disables. |
| `sn_rust_profile_get_sampling_rate` | Read the current sampling interval. |
| `sn_rust_profile_snapshot_begin` / `_count` / `_get` / `_end` | RAII-style enumeration of currently-live sampled allocations. |
| `sn_rust_profile_streaming_start` / `_stop` | Register a `void(*)(const SnRustProfileRawSample*)` callback that receives every sample as it occurs. |

These are the same exports the Rust crate calls into; see
`src/snmalloc/override/rust.cc` for the full ABI surface and
`src/snmalloc/override/rust.h` for the header layout.

### Rust crate

For Rust applications, the [`snmalloc-rs`](snmalloc-rs/README.md) crate
provides a fully safe wrapper around the C ABI: an RAII snapshot type
([`HeapProfile`](snmalloc-rs/src/profile.rs)), an RAII streaming
session ([`ProfilingSession`](snmalloc-rs/src/streaming.rs)), and an
env-var-driven initializer
([`SnMalloc::init_profiling_from_env`](snmalloc-rs/src/config.rs)) that
lets operators turn profiling on at the command line without
recompiling. See [snmalloc-rs/README.md](snmalloc-rs/README.md#heap-profiling)
for the full Rust API and code samples.

### Output formats

Two viewer formats are supported out of the box from the Rust crate:

- **Folded / collapsed flame-graph format** — one line per unique
  stack, summed weights, consumable by Brendan Gregg's
  [`flamegraph.pl`](https://github.com/brendangregg/FlameGraph), the
  pure-Rust [`inferno-flamegraph`](https://github.com/jonhoo/inferno),
  and the [Speedscope](https://www.speedscope.app/) viewer (via its
  "Brendan Gregg's collapsed stack format" importer).
- **Google `pprof` Profile protobuf** — consumable by `go tool pprof`,
  [Pyroscope](https://pyroscope.io/), [Polar Signals
  Cloud](https://www.polarsignals.com/), [Parca](https://www.parca.dev/),
  and the Datadog continuous profiler. Emitted with two sample axes
  (`alloc_objects`/count and `alloc_space`/bytes).

### Overhead

At the default 512 KiB sampling rate, the profiler adds **<1% throughput
overhead** on the criterion micro-benchmark suite shipped in
[`snmalloc-rs/benches/profile_bench.rs`](snmalloc-rs/benches/profile_bench.rs)
(Phase 7 of the heap-profiling design). The bench measures three
configurations — `profile-off`, `profile-on-inactive`, and
`profile-on-active` — and verifies that even the *active* configuration
stays within the 1% budget on the standard sizes. Builds with
`SNMALLOC_PROFILE=OFF` are bit-for-bit identical on the hot path to
those without any profiling code at all.

# Contributing

This project welcomes contributions and suggestions.  Most contributions require you to agree to a
Contributor License Agreement (CLA) declaring that you have the right to, and actually do, grant us
the rights to use your contribution. For details, visit https://cla.microsoft.com.

When you submit a pull request, a CLA-bot will automatically determine whether you need to provide
a CLA and decorate the PR appropriately (e.g., label, comment). Simply follow the instructions
provided by the bot. You will only need to do this once across all repos using our CLA.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
