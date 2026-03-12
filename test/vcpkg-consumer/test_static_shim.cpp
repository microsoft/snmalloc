/**
 * Minimal vcpkg integration test for the snmallocshim-static target.
 * Verifies that find_package(snmalloc) + snmalloc::snmallocshim-static works.
 * The static shim replaces malloc/free with the snmalloc implementation.
 */
#include <stdlib.h>

extern "C"
{
  void *sn_malloc(size_t);
  void sn_free(void *);
}

int main()
{
  void* p = sn_malloc(64);
  if (p == nullptr)
    return 1;
  sn_free(p);
  return 0;
}
