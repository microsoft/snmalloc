# snmalloc-rs
MSVC/MinGW/Linux/MacOS: [![travis ci](https://www.travis-ci.org/SchrodingerZhu/snmalloc-rs.svg?branch=master)](https://travis-ci.com/SchrodingerZhu/snmalloc-rs)

FreeBSD: [![Build Status](https://api.cirrus-ci.com/github/SchrodingerZhu/snmalloc-rs.svg)](https://cirrus-ci.com/github/SchrodingerZhu/snmalloc-rs)

`snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as a global allocator for rust.
snmalloc is a research allocator. Its key design features are:

- Memory that is freed by the same thread that allocated it does not require any synchronising operations.
- Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel message passing scheme to return the memory to the original allocator, where it is recycled.
- The allocator uses large ranges of pages to reduce the amount of meta-data required.

Some old benchmark results are available in the [`snmalloc` paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf).
There are three features defined in this crate:
- `debug`: Enable the `Debug` mode in `snmalloc`.
- `1mib`: Use the `1mib` chunk configuration.
- `cache-friendly`: Make the allocator more cache friendly (setting `CACHE_FRIENDLY_OFFSET` to `64` in building the library).

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
`mingw` version is only tested on nighly branch. Due to the complexity of locating GNU libraries on Windows environment,
the library requests you to provide a `MINGW64_BIN` environment variable during compiling. Since `GCC` does not provide a option for us 
to link `libatomic` statically, I have to use dynamic linking. Hence, please make sure the following libs are in your `PATH`:
- `winpthread`
- `atomic`
- `stdc++`
- `gcc_s` 
This is the best thing I can do for current stage, if you have any better solution, please do help me to provide a better support for
`MinGW`
## Changelog
### 0.2.7
- partially fixed `mingw`
- **upstream** remote dealloc refactor (higher performance)
- **upstream** remove extra assertions
### 0.2.6
- fix `macos`/`freebsd ` support
- add more ci tests
- mark the `mingw` problem
