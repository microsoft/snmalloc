#include <snmalloc.h>

int main()
{
#ifndef SNMALLOC_PASS_THROUGH // This test depends on snmalloc internals
  snmalloc::Alloc& a = snmalloc::ThreadAlloc::get();
  bool result;

  auto r = a.alloc(16);

  snmalloc::debug_check_empty(snmalloc::Globals::get_handle(), &result);
  if (result != false)
  {
    abort();
  }

  a.dealloc(r);

  snmalloc::debug_check_empty(snmalloc::Globals::get_handle(), &result);
  if (result != true)
  {
    abort();
  }

  r = a.alloc(16);

  snmalloc::debug_check_empty(snmalloc::Globals::get_handle(), &result);
  if (result != false)
  {
    abort();
  }

  a.dealloc(r);

  snmalloc::debug_check_empty(snmalloc::Globals::get_handle(), &result);
  if (result != true)
  {
    abort();
  }
#endif
}