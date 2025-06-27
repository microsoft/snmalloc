# Windows snmalloc override

This directory contains an implementation of the snmalloc allocator that
overrides the default Windows allocator for the C-runtime (CRT).
The implementation uses the detours library to intercept calls to the CRT
and redirects them to snmalloc.

# Building

Build the cmake projects in this directory.  From the root of the repository, do

```bash
cmake -B build_detours -S src/windows
cmake --build build_detours
```

This will install the detours library using vcpkg and then build the snmalloc
static library, and a DLL that automatically load snmalloc as the CRT allocator.

# Using

## DLL 
To use the snmalloc allocator in your Windows application, you need to link against the `snmallocdetourslib.dll` that was built in the previous step.
You can do this by adding the following line to your CMakeLists.txt:

```cmake
target_link_libraries(your_target_name PRIVATE snmallocdetourslib)
target_link_options(snmallocdetoursexample PRIVATE "/INCLUDE:is_snmalloc_detour")
```

The second line is necessary to ensure that the linker does not optimize away the DLL import.


## Static Library

To use the snmalloc allocator as a static library, you can link against the `snmallocdetourslib.lib` file that was built in the previous step.
You can do this by adding the following line to your CMakeLists.txt:

```cmake
target_link_libraries(your_target_name PRIVATE snmallocdetourslib_static)
```

Then you need to cause the detours routine to be run by adding the following lines to a C++ source file:

```cpp
#include "detours.h"
#pragma warning(push)
#pragma warning(disable : 4075)
#pragma init_seg(".CRT$XCV")
static SnmallocDetour snmalloc_detour;
#pragma warning(pop)
```

This causes the detours code to be run early in the program startup.

## Locally scoped detour

Finally, you can use the detour in a locally scoped manner.
This requires linking against the `snmallocdetourslib.lib`, and then can be used as:

```cpp
#include "detours.h"


void my_function()
{
  SnmallocDetour snmalloc_detour;
  // snmalloc is now the CRT allocator for this function
  // ...
} // snmalloc_detour goes out of scope and the CRT allocator is restored
```

Upon exiting the scope of `snmalloc_detour`, the CRT allocator will be restored to its original state.


# Status

This implementation is currently in an alpha state, and is not yet suitable for production use.

Significant testing is required to ensure that it works for a range of applications.
If you sucessfully use this in your application, please let us know by commenting on #700.
