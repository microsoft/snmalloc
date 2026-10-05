/**
 * DLL that uses snmalloc, for the dll_teardown test.
 */

#include <snmalloc/snmalloc.h>

extern "C" __declspec(dllexport) void* dll_teardown_alloc(size_t size)
{
  return snmalloc::alloc(size);
}

extern "C" __declspec(dllexport) void dll_teardown_dealloc(void* p)
{
  snmalloc::dealloc(p);
}
