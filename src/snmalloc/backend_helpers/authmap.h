#pragma once

#include "commonconfig.h"
#include "pagemapregisterrange.h"

namespace snmalloc
{
  /**
   * A dummy authmap that simply passes pointers through. For use on
   * non-StrictProvenance architectures.
   */
  struct DummyAuthmap
  {
    static SNMALLOC_FAST_PATH void init() {}

    static SNMALLOC_FAST_PATH bool register_range(capptr::Arena<void>, size_t)
    {
      return true;
    }

    template<bool potentially_out_of_range = false>
    static SNMALLOC_FAST_PATH capptr::Arena<void> amplify(capptr::Alloc<void> c)
    {
      return capptr::Arena<void>::unsafe_from(c.unsafe_ptr());
    }

    /**
     * Address-keyed sibling of `amplify`: returns a capability with
     * address `a` and (on real capability hardware) the registered
     * arena's permissions. The non-StrictProvenance pass-through
     * variant simply fabricates a pointer at `a`.
     */
    template<bool potentially_out_of_range = false>
    static SNMALLOC_FAST_PATH capptr::Arena<void>
    amplify_from_address(address_t a)
    {
      return capptr::Arena<void>::unsafe_from(reinterpret_cast<void*>(a));
    }
  };

  /**
   * Wrap a concrete Pagemap to store Arena pointers, and use these when
   * amplifying a pointer.
   */
  template<typename ConcreteMap>
  struct BasicAuthmap
  {
    static_assert(
      stl::is_same_v<capptr::Arena<void>, typename ConcreteMap::EntryType>,
      "BasicAuthmap's ConcreteMap must have capptr::Arena<void> element type!");

  private:
    SNMALLOC_REQUIRE_CONSTINIT
    static inline ConcreteMap concreteAuthmap;

  public:
    static SNMALLOC_FAST_PATH void init()
    {
      concreteAuthmap.template init</* randomize_location */ false>();
    }

    static SNMALLOC_FAST_PATH bool
    register_range(capptr::Arena<void> base, size_t size)
    {
      concreteAuthmap.register_range(address_cast(base), size);

      address_t base_addr = address_cast(base);
      for (address_t a = base_addr; a < base_addr + size;
           a += ConcreteMap::GRANULARITY)
      {
        concreteAuthmap.set(a, base);
      }
      return true;
    }

    template<bool potentially_out_of_range = false>
    static SNMALLOC_FAST_PATH capptr::Arena<void> amplify(capptr::Alloc<void> c)
    {
      return Aal::capptr_rebound(
        concreteAuthmap.template get<potentially_out_of_range>(address_cast(c)),
        c);
    }

    /**
     * Address-keyed sibling of `amplify`: returns a capability at
     * address `a` with the registered arena's permissions, suitable
     * for cases where the caller holds only an integer address (for
     * example, in-band tree-node access in `InplaceRep`). The
     * authmap is set once per arena registration and never mutated
     * thereafter, so this lookup is safe under concurrent allocator
     * activity.
     */
    template<bool potentially_out_of_range = false>
    static SNMALLOC_FAST_PATH capptr::Arena<void>
    amplify_from_address(address_t a)
    {
      auto arena = concreteAuthmap.template get<potentially_out_of_range>(a);
      return pointer_offset(arena, a - address_cast(arena));
    }
  };

  /**
   * Pick between the two above implementations based on StrictProvenance
   */
  template<typename CA>
  using DefaultAuthmap = stl::conditional_t<
    aal_supports<StrictProvenance>,
    BasicAuthmap<CA>,
    DummyAuthmap>;

} // namespace snmalloc
