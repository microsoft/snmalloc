#include <snmalloc.h>

int main()
{
#ifndef SNMALLOC_PASS_THROUGH // This test depends on snmalloc internals
  snmalloc::Alloc* a = snmalloc::ThreadAlloc::get();
  bool result;

  auto r = a->alloc(16);

  snmalloc::current_alloc_pool()->debug_check_empty(&result);
  if (result != false)
  {
    abort();
  }

  a->dealloc(r);

  snmalloc::current_alloc_pool()->debug_check_empty(&result);
  if (result != true)
  {
    abort();
  }

  r = a->alloc(16);

  snmalloc::current_alloc_pool()->debug_check_empty(&result);
  if (result != false)
  {
    abort();
  }

  a->dealloc(r);

  snmalloc::current_alloc_pool()->debug_check_empty(&result);
  if (result != true)
  {
    abort();
  }
#endif
}