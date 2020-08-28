#include "malloc-extensions.h"

#include "../snmalloc.h"

using namespace snmalloc;

void get_malloc_info_v1(malloc_info_v1* stats)
{
  auto next_memory_usage = default_memory_provider().memory_usage();
  stats->current_memory_usage = next_memory_usage.first;
  stats->peak_memory_usage = next_memory_usage.second;
}