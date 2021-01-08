#pragma once

#include "../mem/baseslab.h"
#include "remoteallocator.h"

#include <type_traits>

namespace snmalloc
{
  class AllocslabStaticChecks;

  class Allocslab
  {
  protected:
    friend class AllocslabStaticChecks;
    friend class Superslab;
    friend class Mediumslab;

    // Maintain first member for pointer interconvertability
    Baseslab base;
    RemoteAllocator* allocator;

  public:
    RemoteAllocator* get_allocator()
    {
      return allocator;
    }

    static Allocslab* get(void* a)
    {
      return pointer_align_down<SUPERSLAB_SIZE, Allocslab>(a);
    }
  };

  class AllocslabStaticChecks
  {
    static_assert(
      std::is_standard_layout_v<Allocslab>, "Allocslab not standard layout");

#ifdef __cpp_lib_is_pointer_interconvertible
    static_assert(
      std::is_pointer_interconvertible_with_class(&Allocslab::base),
      "Allocslab not interconvertible with allocslab");
#endif
  };
} // namespace snmalloc
