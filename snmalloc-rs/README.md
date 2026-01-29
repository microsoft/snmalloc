# snmalloc-rs

CI: [![Actions Status](https://github.com/schrodingerzhu/snmalloc-rs/workflows/rust/badge.svg)](https://github.com/microsoft/snmalloc-rs/actions)


`snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as
a global allocator for rust. snmalloc is a research allocator. Its key design features are:

- Memory that is freed by the same thread that allocated it does not require any synchronising operations.
- Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel
  message passing scheme to return the memory to the original allocator, where it is recycled.
- The allocator uses large ranges of pages to reduce the amount of meta-data required.

Some old benchmark results are available in
the [`snmalloc` paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf). 

There are three features defined in this crate:

- `debug`: Enable the `Debug` mode in `snmalloc`.
- ~~`1mib`: Use the `1mib` chunk configuration. From `0.2.17`, this is set as a default feature~~ (removed since 0.3.0)
- ~~`16mib`: Use the `16mib` chunk configuration.~~ (removed since 0.3.0)
- ~~`cache-friendly`: Make the allocator more cache friendly (setting `CACHE_FRIENDLY_OFFSET` to `64` in building the
  library).~~ (removed since 0.3.0)
- `native-cpu`: Optimize `snmalloc` for the native CPU of the host machine. (this is not a default behavior
  since `0.2.14`)
- `qemu`: Workaround `madvise` problem of QEMU environment
- ~~`stats`: Enable statistics~~ (removed since 0.3.0)
- `local_dynamic_tls`: Workaround cannot allocate memory in static tls block
- `build_cc`: Use of cc crate instead of cmake (cmake still default) as builder (more platform agnostic)
- ~~`usecxx20`: Enable C++20 standard if available~~ (removed since 0.3.0)
- `usecxx17`: Use C++17 standard
- `check`: Enable extra checks to improve security, see upstream [security docs](https://github.com/microsoft/snmalloc/tree/main/docs/security).
  Note that the `memcpy` protection is not enabled in Rust.
- `win8compat`: Improve compatibility for old Windows platforms (removing usages of `VirtualAlloc2` and other new APIs)
- `lto`: Links with InterProceduralOptimization/LinkTimeOptimization
- `notls`: Enables to be loaded dynamically, thus disable tls.
- `stats`: Enables allocation statistics.
- `libc-api`: Enables libc API backed by snmalloc.

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
