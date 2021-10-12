#include <iostream>

#ifdef SNMALLOC_PASS_THROUGH
// This test does not make sense in pass-through
int main()
{
  return 0;
}
#else

// #  define SNMALLOC_TRACING

#  include <backend/backend.h>
#  include <snmalloc_core.h>

// Specify type of allocator
#  define SNMALLOC_PROVIDE_OWN_CONFIG
namespace snmalloc
{
  class CustomGlobals : public BackendAllocator<Pal, false>
  {
  public:
    using GlobalPoolState = PoolState<CoreAllocator<CustomGlobals>>;

  private:
    using Backend = BackendAllocator<Pal, false>;
    SNMALLOC_REQUIRE_CONSTINIT
    inline static ChunkAllocatorState slab_allocator_state;

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
    static constexpr Flags Options = []() constexpr
    {
      Flags opts = {};
      opts.QueueHeadsAreTame = false;
      return opts;
    }
    ();

    static ChunkAllocatorState&
    get_slab_allocator_state(Backend::LocalState* = nullptr)
    {
      return slab_allocator_state;
    }

    static GlobalPoolState& pool()
    {
      return alloc_pool;
    }

    static void register_clean_up()
    {
      snmalloc::register_clean_up();
    }

    static inline bool domesticate_trace;
    static inline size_t domesticate_count;
    static inline uintptr_t* domesticate_patch_location;
    static inline uintptr_t domesticate_patch_value;

    /* Verify that a pointer points into the region managed by this config */
    template<typename T, SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    static SNMALLOC_FAST_PATH CapPtr<
      T,
      typename B::template with_wildness<capptr::dimension::Wildness::Tame>>
    capptr_domesticate(typename Backend::LocalState*, CapPtr<T, B> p)
    {
      domesticate_count++;

      if (domesticate_trace)
      {
        std::cout << "Domesticating " << p.unsafe_ptr()
#  if __has_builtin(__builtin_return_address)
                  << " from " << __builtin_return_address(0)
#  endif
                  << std::endl;
      }

      if (
        domesticate_patch_location != nullptr &&
        p.template as_reinterpret<uintptr_t>().unsafe_ptr() ==
          domesticate_patch_location)
      {
        std::cout << "Patching over corruption" << std::endl;
        *domesticate_patch_location = domesticate_patch_value;
        snmalloc::CustomGlobals::domesticate_patch_location = nullptr;
      }

      return CapPtr<
        T,
        typename B::template with_wildness<capptr::dimension::Wildness::Tame>>(
        p.unsafe_ptr());
    }
  };

  using Alloc = LocalAllocator<CustomGlobals>;
}

#  define SNMALLOC_NAME_MANGLE(a) test_##a
#  include "../../../override/malloc.cc"

int main()
{
  snmalloc::CustomGlobals::init(); // init pagemap
  snmalloc::CustomGlobals::domesticate_count = 0;

  LocalEntropy entropy;
  entropy.init<Pal>();
  key_global = FreeListKey(entropy.get_free_list_key());

  auto alloc1 = new Alloc();

  auto p = alloc1->alloc(48);
  std::cout << "Allocated p " << p << std::endl;

  // Put that free object on alloc1's remote queue
  auto alloc2 = new Alloc();
  alloc2->dealloc(p);
  alloc2->flush();

  // Clobber the linkage but not the back pointer
  snmalloc::CustomGlobals::domesticate_patch_location =
    static_cast<uintptr_t*>(p);
  snmalloc::CustomGlobals::domesticate_patch_value =
    *static_cast<uintptr_t*>(p);
  memset(p, 0xA5, sizeof(void*));

  snmalloc::CustomGlobals::domesticate_trace = true;
  snmalloc::CustomGlobals::domesticate_count = 0;

  auto q = alloc1->alloc(56);
  std::cout << "Allocated q " << q << std::endl;

  snmalloc::CustomGlobals::domesticate_trace = false;

  /*
   * Expected domestication calls in the above message passing:
   *
   *   - On !QueueHeadsAreTame builds only, RemoteAllocator::dequeue
   *     domesticating the front pointer (to the initial stub)
   *
   *   - RemoteAllocator::dequeue domesticating the stub's next pointer (p)
   *
   *   - On !QueueHeadsAreTame builds only, RemoteAllocator::dequeue
   *     domesticating the front pointer (to p, this time)
   *
   *   - RemoteAllocator::dequeue domesticating nullptr (p is the last message)
   *
   *   - Metaslab::alloc_free_list, domesticating the successor object in the
   *     newly minted freelist::Iter (i.e., the thing that would be allocated
   *     after q).
   */
  static constexpr size_t expected_count =
    snmalloc::CustomGlobals::Options.QueueHeadsAreTame ? 3 : 5;
  SNMALLOC_CHECK(snmalloc::CustomGlobals::domesticate_count == expected_count);

  // Prevent the allocators from going out of scope during the above test
  alloc1->flush();
  alloc2->flush();

  return 0;
}

#endif
