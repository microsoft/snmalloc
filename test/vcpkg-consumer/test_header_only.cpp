/**
 * Minimal vcpkg integration test for the snmalloc header-only target.
 * Verifies that find_package(snmalloc) + snmalloc::snmalloc works.
 *
 * The installed INSTALL_INTERFACE include path is "include/snmalloc",
 * so we include snmalloc.h directly (not snmalloc/snmalloc.h).
 */
#include <snmalloc.h>

int main()
{
  void* p = snmalloc::libc::malloc(64);
  if (p == nullptr)
    return 1;
  snmalloc::libc::free(p);
  return 0;
}
