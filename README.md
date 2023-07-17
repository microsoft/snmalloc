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
