#include "malloc-extensions.h"

#include "../snmalloc.h"

using namespace snmalloc;

void get_malloc_info_v1(malloc_info_v1* stats)
{
  auto curr = Globals::get_current_usage();
  auto peak = Globals::get_peak_usage();
  stats->current_memory_usage = curr;
  stats->peak_memory_usage = peak;
}
