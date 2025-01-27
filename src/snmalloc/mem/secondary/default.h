#pragma once

#include "snmalloc/ds/ds.h"

#include <stddef.h>

namespace snmalloc
{
  class DefaultSecondaryAllocator
  {
    SNMALLOC_FAST_PATH
    static void* allocate([[maybe_unused]] size_t size)
    {
      return nullptr;
    }

    SNMALLOC_FAST_PATH
    static void deallocate(void* pointer)
    {
      // If pointer is not null, then dealloc has been call on something
      // it shouldn't be called on.
      // TODO: Should this be tested even in the !CHECK_CLIENT case?
      snmalloc_check_client(
        mitigations(sanity_checks),
        pointer == nullptr,
        "Not allocated by snmalloc.");
    }
  };
} // namespace snmalloc
