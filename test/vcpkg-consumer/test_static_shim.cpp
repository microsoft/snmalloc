/**
 * Minimal vcpkg integration test for the snmallocshim-static target.
 * Verifies that find_package(snmalloc) + snmalloc::snmallocshim-static works.
 * The static shim replaces malloc/free with the snmalloc implementation.
 */
#include <stdlib.h>

int main()
{
  void* p = malloc(64);
  if (p == nullptr)
    return 1;
  free(p);
  return 0;
}
