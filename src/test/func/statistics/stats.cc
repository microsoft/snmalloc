#include <snmalloc/snmalloc.h>

int main()
{
#ifndef SNMALLOC_PASS_THROUGH // This test depends on snmalloc internals
  snmalloc::Alloc& a = snmalloc::ThreadAlloc::get();
  bool result;

  auto r = a.alloc(16);

   snmalloc::print_alloc_stats<snmalloc::Globals>();

  snmalloc::debug_check_empty<snmalloc::Globals>(&result);
  if (result != false)
  {
    abort();
  }

  snmalloc::print_alloc_stats<snmalloc::Globals>();

   a.dealloc(r);

  snmalloc::print_alloc_stats<snmalloc::Globals>();

  snmalloc::debug_check_empty<snmalloc::Globals>(&result);
  if (result != true)
  {
    abort();
  }

  snmalloc::print_alloc_stats<snmalloc::Globals>();

  r = a.alloc(16);

  snmalloc::print_alloc_stats<snmalloc::Globals>();

  snmalloc::debug_check_empty<snmalloc::Globals>(&result);
  if (result != false)
  {
    abort();
  }

  snmalloc::print_alloc_stats<snmalloc::Globals>();

  a.dealloc(r);

  snmalloc::print_alloc_stats<snmalloc::Globals>();

  snmalloc::debug_check_empty<snmalloc::Globals>(&result);
  if (result != true)
  {
    abort();
  }

  snmalloc::print_alloc_stats<snmalloc::Globals>();
#endif
}
