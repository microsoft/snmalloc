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

# Building on Windows

The Windows build currently depends on Visual Studio 2017.
To build with Visual Studio:

```
mkdir build
cd build
cmake -G "Visual Studio 15 2017 Win64" ..
cmake --build . --config Debug
cmake --build . --config Release
cmake --build . --config RelWithDebInfo
```

You can also omit the last three steps and build from the IDE.
Visual Studio builds use a separate directory to keep the binaries for each
build configuration.

Alternatively, you can follow the steps in the next section to build with Ninja
using the Visual Studio compiler.

# Building on UNIX-like platforms

snmalloc has platform abstraction layers for XNU (macOS, iOS, and so on),
FreeBSD, NetBSD, OpenBSD, and Linux and is expected to work out of the box on
these systems.
Please open issues if it does not.
Note that NetBSD, by default, ships with a toolchain that emits calls to
`libatomic` but does not ship `libatomic`.
To use snmalloc on NetBSD, you must either acquire a `libatomic` implementation
(for example, from the GCC or LLVM project) or compile with clang.

snmalloc has very few dependencies, CMake, Ninja, Clang 6.0 or later and a C++17
standard library.
Building with GCC is currently not recommended because GCC emits calls to
libatomic for 128-bit atomic operations.

To build a debug configuration:
```
mkdir build
cd build
cmake -G Ninja .. -DCMAKE_BUILD_TYPE=Debug
ninja
```
To build a release configuration:
```
mkdir build
cd build
cmake -G Ninja .. -DCMAKE_BUILD_TYPE=Release
ninja
```
To build with optimizations on, but with debug information:
```
mkdir build
cd build
cmake -G Ninja .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja
```

On ELF platforms, the build produces a binary `libsnmallocshim.so`.
This file can be 
`LD_PRELOAD`ed to use the allocator in place of the system allocator, for
example, you can run the build script using the snmalloc as the allocator for
your toolchain:

```
LD_PRELOAD=/usr/local/lib/libsnmallocshim.so ninja
```

# CMake Feature Flags

These can be added to your cmake command line.

```
-DUSE_SNMALLOC_STATS=ON // Track allocation stats
-DUSE_MEASURE=ON // Measure performance with histograms
```

# Using snmalloc as header-only library

In this section we show how to compile snmalloc into your project such that it replaces the standard allocator functions such as free and malloc. The following instructions were tested with CMake and Clang running on Ubuntu 18.04.

Add these lines to your CMake file.

```cmake
set(SNMALLOC_ONLY_HEADER_LIBRARY ON)
add_subdirectory(snmalloc EXCLUDE_FROM_ALL)
```

In addition make sure your executable is compiled to support 128 bit atomic operations. This may require you to add the following to your CMake file.

```cmake
target_link_libraries([lib_name] PRIVATE snmalloc_lib)
```

You will also need to compile the relevant parts of snmalloc itself. Create a new file with the following contents and compile it with the rest of your application.

```c++
#define NO_BOOTSTRAP_ALLOCATOR

#include "snmalloc/src/override/malloc.cc"
```

# Porting snmalloc to a new platform

All of the platform-specific logic in snmalloc is isolated in the [Platform
Abstraction Layer (PAL)](src/pal).
To add support for a new platform, you will need to implement a new PAL for
your system.

The PAL must implement the following methods:

```c++
void error(const char* const str) noexcept;
```
Report a fatal error and exit.

```c++
void notify_not_using(void* p, size_t size) noexcept;
```
Notify the system that the range of memory from `p` to `p` + `size` is no
longer in use, allowing the underlying physical pages to recycled for other
purposes.

```c++
template<ZeroMem zero_mem>
void notify_using(void* p, size_t size) noexcept;
```
Notify the system that the range of memory from `p` to `p` + `size` is now in use.
On systems that lazily provide physical memory to virtual mappings, this
function may not be required to do anything.
If the template parameter is set to `YesZero` then this function is also
responsible for ensuring that the newly requested memory is full of zeros.

```c++
template<bool page_aligned = false>
void zero(void* p, size_t size) noexcept;
```
Zero the range of memory from `p` to `p` + `size`.
This may be a simple `memset` call, but the `page_aligned` template parameter
allows for more efficient implementations when entire pages are being zeroed.
This function is typically called with very large ranges, so it may be more
efficient to request that the operating system provides background-zeroed
pages, rather than zeroing them synchronously in this call

```c++
template<bool committed>
void* reserve(size_t size, size_t align);
template<bool committed>
void* reserve(size_t size) noexcept;
```
Only one of these needs to be implemented, depending on whether the underlying
system can provide strongly aligned memory regions.
If the system guarantees only page alignment, implement the second and snmalloc
will over-allocate and then trim the requested region.
If the system provides strong alignment, implement the first to return memory
at the desired alignment.

Finally, you need to define a field to indicate the features that your PAL supports:
```c++
static constexpr uint64_t pal_features = ...;
```

These features are defined in the [`PalFeatures`](src/pal/pal_consts.h) enumeration.

There are several partial PALs that can be used when implementing POSIX-like systems:

 - `PALPOSIX` defines a PAL for a POSIX platform using no non-standard features.
 - `PALBSD` defines a PAL for the common set of BSD extensions to POSIX.
 - `PALBSD_Aligned` extends `PALBSD` to provide support for aligned allocation
   from `mmap`, as supported by NetBSD and FreeBSD.

Each of these template classes takes the PAL that inherits from it as a
template parameter.
A purely POSIX-compliant platform could have a PAL as simple as this:

```c++
class PALMyOS : public PALPOSIX<PALMyOS> {}
```

Typically, a PAL will implement at least one of the functions outlined above in
a more-efficient platform-specific way, but this is not required.
Non-POSIX systems will need to implement the entire PAL interface.
The [Windows](src/pal/pal_windows.h), and
[OpenEnclave](src/pal/pal_open_enclave.h) and
[FreeBSD kernel](src/pal/pal_freebsd_kernel.h) implementations give examples of
non-POSIX environments that snmalloc supports.

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
