#include <iostream>

// #  define SNMALLOC_TRACING

#include <snmalloc/backend/backend.h>
#include <snmalloc/backend/standard_range.h>
#include <snmalloc/backend_helpers/backend_helpers.h>
#include <snmalloc/mem/secondary/default.h>
#include <snmalloc/snmalloc_core.h>

// Specify type of allocator
#define SNMALLOC_PROVIDE_OWN_CONFIG

namespace snmalloc
{
  class CustomConfig : public CommonConfig
  {
  public:
    using Pal = DefaultPal;
    using PagemapEntry = DefaultPagemapEntry<NoClientMetaDataProvider>;
    using ClientMeta = NoClientMetaDataProvider;
    using SecondaryAllocator = DefaultSecondaryAllocator;

  private:
    using ConcretePagemap =
      FlatPagemap<MIN_CHUNK_BITS, PagemapEntry, Pal, false>;

  public:
    using Pagemap = BasicPagemap<Pal, ConcretePagemap, PagemapEntry, false>;

    using ConcreteAuthmap =
      FlatPagemap<MinBaseSizeBits<Pal>(), capptr::Arena<void>, Pal, false>;

    using Authmap = DefaultAuthmap<ConcreteAuthmap>;

  public:
    using Base = Pipe<
      PalRange<Pal>,
      PagemapRegisterRange<Pagemap>,
      PagemapRegisterRange<Authmap>>;

    using LocalState = StandardLocalState<Pal, Pagemap, Base>;

    using GlobalPoolState = PoolState<Allocator<CustomConfig>>;

    using Backend =
      BackendAllocator<Pal, PagemapEntry, Pagemap, Authmap, LocalState>;

  private:
    SNMALLOC_REQUIRE_CONSTINIT
    inline static GlobalPoolState alloc_pool;

  public:
    /*
     * C++, even as late as C++20, has some really quite strict limitations on
     * designated initializers.  However, as of C++17, we can have constexpr
     * lambdas and so can use more of the power of the statement fragment of
     * C++, and not just its initializer fragment, to initialize a non-prefix
     * subset of the flags (in any order, at that).
     */
    static constexpr Flags Options = []() constexpr {
      Flags opts = {};
      opts.QueueHeadsAreTame = false;
      opts.HasDomesticate = true;
      return opts;
    }();

    static GlobalPoolState& pool()
    {
      return alloc_pool;
    }

    static inline bool domesticate_trace;
    static inline size_t domesticate_count;
    static inline uintptr_t* domesticate_patch_location;
    static inline uintptr_t domesticate_patch_value;

    /* Verify that a pointer points into the region managed by this config */
    template<typename T, SNMALLOC_CONCEPT(capptr::IsBound) B>
    static CapPtr<
      T,
      typename B::template with_wildness<capptr::dimension::Wildness::Tame>>
    capptr_domesticate(LocalState*, CapPtr<T, B> p)
    {
      domesticate_count++;

      if (domesticate_trace)
      {
        std::cout << "Domesticating " << p.unsafe_ptr()
#if __has_builtin(__builtin_return_address)
                  << " from " << __builtin_return_address(0)
#endif
                  << std::endl;
      }

      if (
        domesticate_patch_location != nullptr &&
        p.template as_reinterpret<uintptr_t>().unsafe_ptr() ==
          domesticate_patch_location)
      {
        std::cout << "Patching over corruption" << std::endl;
        *domesticate_patch_location = domesticate_patch_value;
        snmalloc::CustomConfig::domesticate_patch_location = nullptr;
      }

      return CapPtr<
        T,
        typename B::template with_wildness<capptr::dimension::Wildness::Tame>>::
        unsafe_from(p.unsafe_ptr());
    }
  };

  using Config = CustomConfig;
}

#define SNMALLOC_NAME_MANGLE(a) test_##a
#include <snmalloc/override/malloc.cc>

int main()
{
  static constexpr bool pagemap_randomize =
    mitigations(random_pagemap) & !aal_supports<StrictProvenance>;

  snmalloc::CustomConfig::Pagemap::concretePagemap.init<pagemap_randomize>();
  snmalloc::CustomConfig::Authmap::init();
  snmalloc::CustomConfig::domesticate_count = 0;

  LocalEntropy entropy;
  entropy.init<DefaultPal>();
  entropy.make_free_list_key(RemoteAllocator::key_global);
  entropy.make_free_list_key(freelist::Object::key_root);

  ScopedAllocator alloc1;

  // Allocate from alloc1; the size doesn't matter a whole lot, it just needs to
  // be a small object and so definitely owned by this allocator rather.
  auto p = alloc1->alloc(16);
  auto q = alloc1->alloc(32);
  std::cout << "Allocated p " << p << std::endl;
  std::cout << "Allocated q " << q << std::endl;

  // Put that free object on alloc1's remote queue
  ScopedAllocator alloc2;
  alloc2->dealloc(p);
  alloc2->dealloc(q);
  alloc2->flush();

  // Clobber the linkage but not the back pointer
  snmalloc::CustomConfig::domesticate_patch_location =
    static_cast<uintptr_t*>(p);
  snmalloc::CustomConfig::domesticate_patch_value = *static_cast<uintptr_t*>(p);
  // TODO This fails when we add a second remote deallocation.
  //  Is this a sign that we are missing places where we should domesticate?
  // memset(p, 0xA5, sizeof(void*));

  snmalloc::CustomConfig::domesticate_trace = true;
  snmalloc::CustomConfig::domesticate_count = 0;

  // Open a new slab, so that slow path will pick up the message queue.  That
  // means this should be a sizeclass we've not used before, even internally.
  auto r = alloc1->alloc(512);
  std::cout << "Allocated r " << r << std::endl;

  snmalloc::CustomConfig::domesticate_trace = false;

  /*
   * TODO This is currently disabled.  The PR changes the expected number of
   * domestication calls.  The combination of this change with BatchIt means
   * it is hard to predict how many domestications call will be made.
   *
   * Expected domestication calls in the above message passing:
   *
   *   - On !QueueHeadsAreTame builds only, RemoteAllocator::dequeue
   *     domesticating the front pointer (to the initial stub)
   *
   *   - RemoteAllocator::dequeue domesticating the stub's next pointer (p)
   *
   *   - FrontendMetaData::alloc_free_list, domesticating the successor object
   * in the newly minted freelist::Iter (i.e., the thing that would be allocated
   *     after r).
   */
  std::cout << "domesticate_count = "
            << snmalloc::CustomConfig::domesticate_count << std::endl;
  // static constexpr size_t expected_count =
  //   snmalloc::CustomConfig::Options.QueueHeadsAreTame ? 5 : 6;
  // SNMALLOC_CHECK(snmalloc::CustomConfig::domesticate_count ==
  // expected_count);

  return 0;
}