# snmalloc-rs
`snmalloc-rs` provides a wrapper for [`microsoft/snmalloc`](https://github.com/microsoft/snmalloc) to make it usable as a global allocator for rust.
snmalloc is a research allocator. Its key design features are:
- Memory that is freed by the same thread that allocated it does not require any synchronising operations.
- Freeing memory in a different thread to initially allocated it, does not take any locks and instead uses a novel message passing scheme to return the memory to the original allocator, where it is recycled.
- The allocator uses large ranges of pages to reduce the amount of meta-data required.

The benchmark is available at the [paper](https://github.com/microsoft/snmalloc/blob/master/snmalloc.pdf) of `snmalloc`
There are three features defined in this crate:
- `debug`: Enable the `Debug` mode in `snmalloc`.
- `1mib`: Use the `1mib` chunk configuration.
- `cache-friendly`: Make the allocator more cache friendly (setting `CACHE_FRIENDLY_OFFSET` to `64` in building the library).

To use `snmalloc-rs` add it as a dependency:
```toml
# Cargo.toml
[dependencies]
snmalloc-rs = "0.1.0"
```

To set `SnMalloc` as the global allocator add this to your project:
```rust
#[global_allocator]
static ALLOC: snmalloc_rs::SnMalloc = snmalloc_rs::SnMalloc;
```