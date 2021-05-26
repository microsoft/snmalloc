#include <backend/backend.h>
#include <backend/slaballocator.h>
#include <ds/defines.h>
#include <iostream>

namespace snmalloc
{
  struct StaticHandle
  {
    using Meta = MetaEntry;
    using Pal = DefaultPal;

    inline static AddressSpaceManager<Pal> address_space;
    inline static AddressSpaceManager<Pal> meta_address_space;
    inline static FlatPagemap<MIN_CHUNK_BITS, Meta, false> pagemap;
    inline static SlabAllocatorState slab_allocator_state;

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

    static void init()
    {
      // Need to initialise pagemap.
      pagemap.init(&meta_address_space);
    }
  };

  class Globals : public snmalloc::StaticHandle
  {
  public:
    static Globals get_handle()
    {
      return {};
    }
  };
}

SNMALLOC_SLOW_PATH void dealloc_object_slow(
  snmalloc::Metaslab* meta,
  snmalloc::LocalEntropy& entropy,
  snmalloc::SlabList& sl)
{
  UNUSED(entropy);
  if (meta->is_full())
  {
    auto allocated = snmalloc::get_slab_capacity(meta->sizeclass(), false);
    //  Remove trigger threshold from how many we need before we have fully
    //  freed the slab.
    meta->needed() = allocated - meta->threshold_for_waking_slab(false);

    // We are not on the sizeclass list.
    if (meta->needed() == 0)
    {
      std::cout << "Slab is full" << std::endl;
      return;
    }

    sl.insert_prev(meta);
    std::cout << "Slab is woken up" << std::endl;

    return;
  }

  std::cout << "Slab is full" << std::endl;
}

template<typename SharedStateHandle>
SNMALLOC_SLOW_PATH void dealloc_object(
  SharedStateHandle h,
  void* p,
  snmalloc::LocalEntropy& entropy,
  snmalloc::SlabList& sl)
{
  auto meta =
    snmalloc::BackendAllocator::get_meta_data(h, snmalloc::address_cast(p))
      .meta;
  SNMALLOC_ASSERT(!meta->is_unused());

  auto cp = snmalloc::CapPtr<snmalloc::FreeObject, snmalloc::CBAlloc>(
    (snmalloc::FreeObject*)p);

  // Update the head and the next pointer in the free list.
  meta->free_queue.add(cp, entropy);

  if (likely(!meta->return_object()))
    return;
  dealloc_object_slow(meta, entropy, sl);
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);
  using BA = snmalloc::BackendAllocator;
  snmalloc::Globals::init();
  auto h = snmalloc::Globals::get_handle();

  snmalloc::MetaEntry m1{nullptr, nullptr};
  auto p = BA::alloc_slab(h, snmalloc::MIN_CHUNK_SIZE, m1);
  std::cout << "Chunk " << p.unsafe_capptr << std::endl;

  std::cout << "Get meta-data " << BA::get_meta_data(h, address_cast(p)).meta
            << std::endl;

  BA::set_meta_data(
    h, address_cast(p), {BA::alloc_meta_data<snmalloc::Metaslab>(h), nullptr});

  std::cout << "Get meta-data " << BA::get_meta_data(h, address_cast(p)).meta
            << std::endl;

  snmalloc::MetaEntry m2{nullptr, nullptr};
  m2.meta = BA::alloc_meta_data<snmalloc::Metaslab>(h);
  auto q = BA::alloc_slab(h, snmalloc::MIN_CHUNK_SIZE * 2, m2);
  std::cout << "Chunk " << q.unsafe_capptr << std::endl;

  std::cout << "Get meta-data " << BA::get_meta_data(h, address_cast(q)).meta
            << std::endl;

  snmalloc::FreeListIter fl;
  snmalloc::LocalEntropy entropy;
  entropy.template init<snmalloc::Globals::Pal>();
  auto first_alloc =
    snmalloc::SlabAllocator::alloc(h, 27, /*remote*/ nullptr, fl, entropy);
  std::cout << "First alloc " << first_alloc << std::endl;

  snmalloc::SlabList sl;

  dealloc_object(h, first_alloc, entropy, sl);

  int count = 1;
  while (!fl.empty())
  {
    auto alloc = fl.take(entropy).unsafe_capptr;
    std::cout << "alloc " << alloc << std::endl;

    dealloc_object(h, alloc, entropy, sl);

    count++;
  }
  std::cout << "Allocated " << count << " objects." << std::endl;
}