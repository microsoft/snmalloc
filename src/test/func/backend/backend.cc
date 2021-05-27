#include "backend/backend.h"

#include "backend/slaballocator.h"
#include "mem/fastalloc.h"
#include "mem/pool.h"

#include <ds/defines.h>
#include <iostream>

namespace snmalloc
{
  struct Globals
  {
    using Meta = MetaEntry;
    using Pal = DefaultPal;

    inline static AddressSpaceManager<Pal> address_space;
    inline static AddressSpaceManager<Pal> meta_address_space;
    inline static FlatPagemap<MIN_CHUNK_BITS, Meta, false> pagemap;
    inline static SlabAllocatorState slab_allocator_state;
    inline static PoolState<CoreAlloc<Globals>> alloc_pool;

    AddressSpaceManager<DefaultPal>& get_meta_address_space()
    {
      return meta_address_space;
    }

    AddressSpaceManager<DefaultPal>& get_object_address_space()
    {
      return address_space;
    }

    FlatPagemap<MIN_CHUNK_BITS, Meta, false>& get_pagemap()
    {
      return pagemap;
    }

    SlabAllocatorState& get_slab_allocator_state()
    {
      return slab_allocator_state;
    }

    static PoolState<CoreAlloc<Globals>>& pool()
    {
      return alloc_pool;
    }

    static constexpr bool IsQueueInline = true;

    // Performs initialisation for this configuration
    // of allocators.  Will be called at most once
    // before any other datastructures are accessed.
    static int init() noexcept
    {
      std::cout << "Run init_impl" << std::endl;
      // Need to initialise pagemap.
      pagemap.init(&meta_address_space);

      return 0;
    }

    // This needs to be a forward reference as the
    // thread local state will need to know about this.
    static void register_clean_up();

    // This is an empty structure as all the state is global
    // for this allocator configuration.
    static constexpr Globals get_handle()
    {
      return {};
    }
  };
}



using Alloc = snmalloc::FastAllocator<snmalloc::Globals>;

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);


  for (size_t i = 0; i < 44; i++)
  {
    Alloc alloc;
    std::cout << "sizeclass: " << i << std::endl;

    for (size_t j = 0; j < 100; j++)
    {
      auto a = alloc.alloc(snmalloc::sizeclass_to_size(i));
      std::cout << "alloc " << j << ": " << a << std::endl;
      alloc.dealloc(a);
    }
    std::cout << "-----------------------------------------------" << std::endl;
    alloc.flush();
  }
}

inline void snmalloc::Globals::register_clean_up()
{
  std::cout << "Register clean up" << std::endl;
}
