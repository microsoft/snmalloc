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
snmalloc-rs = "0.3.8"
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
