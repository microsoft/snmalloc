#pragma once

#include "../mem/baseslab.h"
#include "remoteallocator.h"

namespace snmalloc
{
  class Allocslab : public Baseslab
  {
  protected:
    RemoteAllocator* allocator;

  public:
    RemoteAllocator* get_allocator()
    {
      return allocator;
    }
  };
} // namespace snmalloc
