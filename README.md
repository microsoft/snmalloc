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

Comprehensive details about snmalloc's design can be found in the
[accompanying paper](snmalloc.pdf), and differences between the paper and the
current implementation are [described here](difference.md).
Since writing the paper, the performance of snmalloc has improved considerably.

[![Build Status](https://dev.azure.com/snmalloc/snmalloc/_apis/build/status/Microsoft.snmalloc?branchName=master)](https://dev.azure.com/snmalloc/snmalloc/_build/latest?definitionId=1?branchName=master)

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
