#include <iostream>
#include <pal/pal_noalloc.h>
#include <snmalloc.h>
#ifdef assert
#  undef assert
#endif
#define assert please_use_SNMALLOC_ASSERT

using namespace snmalloc;

/**
 * A single fixed address range allocator configuration
 */
class FixedGlobals : public CommonConfig
{
  inline static AddressSpaceManager<PALNoAlloc<DefaultPal>> address_space;

  inline static FlatPagemap<
    MIN_CHUNK_BITS,
    CommonConfig::Meta,
    true,
    &CommonConfig::default_entry>
    pagemap;

  inline static SlabAllocatorState slab_allocator_state;

  inline static PoolState<CoreAlloc<FixedGlobals>> alloc_pool;

public:
  AddressSpaceManager<PALNoAlloc<DefaultPal>>& get_meta_address_space()
  {
    return address_space;
  }

  AddressSpaceManager<PALNoAlloc<DefaultPal>>& get_object_address_space()
  {
    return address_space;
  }

  FlatPagemap<
    MIN_CHUNK_BITS,
    CommonConfig::Meta,
    true,
    &CommonConfig::default_entry>&
  get_pagemap()
  {
    return pagemap;
  }

  SlabAllocatorState& get_slab_allocator_state()
  {
    return slab_allocator_state;
  }

  PoolState<CoreAlloc<FixedGlobals>>& pool()
  {
    return alloc_pool;
  }

  static constexpr bool IsQueueInline = true;

  // Performs initialisation for this configuration
  // of allocators.  Will be called at most once
  // before any other datastructures are accessed.
  void ensure_init() noexcept
  {
#ifdef SNMALLOC_TRACING
    std::cout << "Run init_impl" << std::endl;
#endif
  }

  static bool is_initialised()
  {
    return true;
  }

  // This needs to be a forward reference as the
  // thread local state will need to know about this.
  static void register_clean_up()
  {
    snmalloc::register_clean_up();
  }

  static void init(CapPtr<void, CBChunk> base, size_t length)
  {
    address_space.add_range(base, length);
    pagemap.init(&address_space, address_cast(base), length);
  }

  static FixedGlobals get_handle()
  {
    return {};
  }
};

using FixedAlloc = FastAllocator<FixedGlobals>;

using namespace snmalloc;
int main()
{
#ifndef SNMALLOC_PASS_THROUGH // Depends on snmalloc specific features
  auto handle = snmalloc::Globals::get_handle();

  auto& address_space = handle.get_object_address_space();
  // 28 is large enough to produce a nested allocator.
  // It is also large enough for the example to run in.
  // For 1MiB superslabs, SUPERSLAB_BITS + 4 is not big enough for the example.
  size_t large_class = 28 - SUPERSLAB_BITS;
  size_t size = bits::one_at_bit(SUPERSLAB_BITS + large_class);
  auto oe_base = address_space.reserve<true>(size);
  auto oe_end = pointer_offset(oe_base, size).unsafe_capptr;
  std::cout << "Allocated region " << oe_base.unsafe_capptr << " - "
            << pointer_offset(oe_base, size).unsafe_capptr << std::endl;

  FixedGlobals fixed_handle;
  FixedGlobals::init(oe_base, size);
  FixedAlloc a(fixed_handle);

  size_t object_size = 128;
  size_t count = 0;
  while (true)
  {
    auto r1 = a.alloc(object_size);
    count += object_size;

    // Run until we exhaust the fixed region.
    // This should return null.
    if (r1 == nullptr)
      break;

    if (oe_base.unsafe_capptr > r1)
    {
      std::cout << "Allocated: " << r1 << std::endl;
      abort();
    }
    if (oe_end < r1)
    {
      std::cout << "Allocated: " << r1 << std::endl;
      abort();
    }
  }

  std::cout << "Total allocated: " << count << " out of " << size << std::endl;
  std::cout << "Overhead: 1/" << (double)size / (double)(size - count)
            << std::endl;

  a.teardown();
#endif
}
