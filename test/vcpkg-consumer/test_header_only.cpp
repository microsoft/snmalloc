/**
 * Minimal vcpkg integration test for the snmalloc header-only target.
 * Verifies that find_package(snmalloc) + snmalloc::snmalloc works.
 */
#include <snmalloc/snmalloc.h>

int main()
{
  void* p = snmalloc::libc::malloc(64);
  if (p == nullptr)
    return 1;
  snmalloc::libc::free(p);
  return 0;
}
