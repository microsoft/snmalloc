#include "malloc-extensions.h"

#include "../snmalloc.h"

using namespace snmalloc;

void get_malloc_info_v1(malloc_info_v1* stats)
{
  auto unused_chunks =
    Globals::get_handle().get_slab_allocator_state().unused_memory();
  auto peak =
    Globals::get_handle().get_object_address_space().peak_memory_usage();
  stats->current_memory_usage = peak - unused_chunks;
  stats->peak_memory_usage = peak;
}