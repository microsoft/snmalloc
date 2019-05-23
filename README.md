# snmalloc

snmalloc is a research allocator.  Its key design features are:

* Memory that is freed by the same thread that allocated it does not require any
  synchronising operations.
* Freeing memory in a different thread to initially allocated it, does not take
  any locks and instead uses a novel message passing scheme to return the
  memory to the original allocator, where it is recycled.
* The allocator uses large ranges of pages to reduce the amount of meta-data
  required.

Details about snmalloc's design can be found in the
[accompanying paper](snmalloc.pdf).

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

# Building on macOS, Linux or FreeBSD
Snmalloc has very few dependencies, CMake, Ninja, Clang 6.0 or later and a C++17
standard library.
Building with GCC is currently not recommended because GCC lacks support for the
`selectany` attribute to specify variables in a COMDAT.  
It will, however, build with GCC-7, but some of global variables will be 
preemptible.

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

On ELF platforms, The build produces a binary `libsnmallocshim.so`.
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
```
set(SNMALLOC_ONLY_HEADER_LIBRARY ON)
add_subdirectory(snmalloc EXCLUDE_FROM_ALL)
```

In addition make sure your executable is compiled to support 128 bit atomic operations. This may require you to add the following to your CMake file.
```
target_link_libraries([lib_name] PRIVATE snmalloc_lib)
```

You will also need to compile the relavent parts of snmalloc itself. Create a new file with the following contents and compile it with the rest of your application.
```
#define NO_BOOTSTRAP_ALLOCATOR

#include "snmalloc/src/override/malloc.cc"
```

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
