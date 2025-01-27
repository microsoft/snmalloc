#pragma once
#ifdef SNMALLOC_ENABLE_GWP_ASAN_INTEGRATION
#  include "snmalloc/mem/secondary/gwp_asan.h"

namespace snmalloc
{
  using SecondaryAllocator = GwpAsanSecondaryAllocator;
} // namespace snmalloc
#else
#  include "snmalloc/mem/secondary/default.h"

namespace snmalloc
{
  using SecondaryAllocator = DefaultSecondaryAllocator;
} // namespace snmalloc
#endif
