#pragma once

#include "baseslab.h"

namespace snmalloc
{
  struct RemoteAllocator;

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
