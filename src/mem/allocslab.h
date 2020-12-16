#pragma once

#include "../mem/baseslab.h"
#include "remoteallocator.h"

namespace snmalloc
{
  struct Allocslab : public Baseslab
  {
    RemoteAllocator* allocator;

    RemoteAllocator* get_allocator()
    {
      return allocator;
    }
  };
} // namespace snmalloc
