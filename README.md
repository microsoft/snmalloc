# snmalloc

snmalloc is a research allocator.  Its key design features are:

* Memory that is freed by the same thread that allocated it does not require any
  synchronising operations.
* Freeing memory in a different thread to initially allocated it, does not take
  any locks and instead uses a novel message passing scheme to return the
  memory to the original allocator, where it is recycled.
* The allocator uses large ranges of pages to reduce the amount of meta-data
  required.

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
Building with GCC is currently not supported because GCC lacks support for the
`selectany` attribute to specify variables in a COMDAT.

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
