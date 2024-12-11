Building snmalloc
=================

snmalloc uses a CMake build system and can be built on many platforms.

# Building on Windows

The Windows build currently depends on at least Visual Studio 2019.
Both Visual Studio 2019 and 2022 are regularly tested in CI.
Additionally, `clang-cl` is also supported and tested by CI.
To build with Visual Studio:

```
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
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

snmalloc has very few dependencies: CMake, Ninja, Clang 6.0 or later and a C++17
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

## Cross Compile for Android
Android is supported out-of-the-box.

To cross-compile the library for arm android, you can simply invoke CMake with the toolchain file and the andorid api settings (for more infomation, check this [document](https://developer.android.com/ndk/guides/cmake)).

For example, you can cross-compile for `arm64-v8a` with the following command:
```
cmake /path/to/snmalloc -DCMAKE_TOOLCHAIN_FILE=${ANDROID_NDK}/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a
```

# CMake Feature Flags

These can be added to your cmake command line.

```
-DUSE_SNMALLOC_STATS=ON // Track allocation stats
```

# Using snmalloc as header-only library

In this section we show how to compile snmalloc into your project such that it replaces the standard allocator functions such as free and malloc. The following instructions were tested with CMake and Clang running on Ubuntu 18.04.

Add these lines to your CMake file.

```cmake
set(SNMALLOC_HEADER_ONLY_LIBRARY ON)
add_subdirectory(snmalloc EXCLUDE_FROM_ALL)
```

In addition make sure your executable is compiled to support 128 bit atomic operations. This may require you to add the following to your CMake file.

```cmake
target_link_libraries([lib_name] PRIVATE snmalloc_lib)
```

You will also need to compile the relevant parts of snmalloc itself. Create a new file with the following contents and compile it with the rest of your application.

```c++
#include "src/snmalloc/override/malloc.cc"
#include "src/snmalloc/override/new.cc"
```

To enable the `reallocarray` symbol export, this can be added to your cmake command line.

```
-DSNMALLOC_NO_REALLOCARRAY=OFF
```

likewise for `reallocarr`.

```
-DSNMALLOC_NO_REALLOCARR=OFF
```

