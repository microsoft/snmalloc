# snmalloc-rs
**Notice: MinGW Build is broken and may not be fixed in a near future. See [this PR](https://github.com/microsoft/snmalloc/pull/217) in the upstream.**

MSVC/MinGW/Linux/MacOS: [![travis ci](https://www.travis-ci.org/SchrodingerZhu/snmalloc-rs.svg?branch=master)](https://travis-ci.com/SchrodingerZhu/snmalloc-rs)

FreeBSD: [![Build Status](https://api.cirrus-ci.com/github/SchrodingerZhu/snmalloc-rs.svg)](https://cirrus-ci.com/github/SchrodingerZhu/snmalloc-rs)

`snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as a global allocator for rust.
snmalloc is a research allocator. Its key design features are:

- Memory that is freed by the same thread that allocated it does not require any synchronising operations.
- Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel message passing scheme to return the memory to the original allocator, where it is recycled.
- The allocator uses large ranges of pages to reduce the amount of meta-data required.

Some old benchmark results are available in the [`snmalloc` paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf). Some recent benchmark results are listed at 
[bench_suite](https://github.com/SchrodingerZhu/bench_suite).
There are three features defined in this crate:

- `debug`: Enable the `Debug` mode in `snmalloc`.
- `1mib`: Use the `1mib` chunk configuration. From `0.2.17`, this is set as a default feature
- `16mib`: Use the `16mib` chunk configuration.
- `cache-friendly`: Make the allocator more cache friendly (setting `CACHE_FRIENDLY_OFFSET` to `64` in building the library).
- `native-cpu`: Optimize `snmalloc` for the native CPU of the host machine. (this is not a default behavior since `0.2.14`)
- `qemu`: workaround `madvise` problem of QEMU environment
- `stats`: enable statistics

**To get the crates compiled, you need to choose either `1mib` or `16mib` to determine the chunk configuration**

To use `snmalloc-rs` add it as a dependency:

```toml
# Cargo.toml
[dependencies]
snmalloc-rs = "0.2"
```

To set `SnMalloc` as the global allocator add this to your project:

```rust
#[global_allocator]
static ALLOC: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;
```

## For MinGW Users

`mingw` version is only tested on nightly branch with MSYS environment. We are using dynamic linking method. 
Hence, please make sure the following libs are in your `PATH`:

- `winpthread`
- `atomic`
- `stdc++`
- `gcc_s` 

**Notice:** since version `0.2.12`, we no longer require you to provide additional environment variables for `mingw` target.

## For Android Cross-Compilation

- `ANDROID_NDK` must be provided as an environment variable
- `ANDROID_PLATFORM` can be passed as an optional environment variable
- `ANDROID_ABI` used by CMake is detected automatically
- feature `android-lld` can be used to set the linker of `snmalloc` to `lld`
- feature `android-shared-std` can be used to set the STL library of `snmalloc` to `c++_shared` (it uses `c++_static` by default)

## Changelog

### 0.2.21

- **upstream** bug fix for using failing to initialise meta-data

### 0.2.20

- **upstream** pass through Haiku build fix. 
- **upstream** fix typo in macro definition for 16MiB shared library shim.
- **upstream** DragonFly support (userland).
- **upstream** natural alignment for USE_MALLOC
- **upstream** fix bug in pagemap when index has many level
- **upstream** add constexpr annotation to align_up/down.  

### 0.2.19

- **upstream** stats
- **upstream** PAL updates and concepts
- **upstream** ddd constexpr annotation to align_up/down
- change macOS CI to follow xcode 12

for older versions, see [CHANGELOG](CHANGELOG.md) 
