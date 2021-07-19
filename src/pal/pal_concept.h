#pragma once

#ifdef __cpp_concepts
#  include "../ds/concept.h"
#  include "pal_consts.h"

#  include <utility>

namespace snmalloc
{
  /**
   * PALs must advertize the bit vector of their supported features and the
   * platform's page size.  This concept enforces that these are indeed
   * constants that fit in the desired types.  (This is subtly different from
   * saying that they are the required types; C++ may handle constants without
   * much regard for their claimed type.)
   */
  template<typename PAL>
  concept ConceptPAL_static_members = requires()
  {
    typename std::integral_constant<uint64_t, PAL::pal_features>;
    typename std::integral_constant<std::size_t, PAL::page_size>;
  };

  /**
   * PALs expose an error reporting function which takes a const C string.
   */
  template<typename PAL>
  concept ConceptPAL_error = requires(const char* const str)
  {
    { PAL::error(str) } -> ConceptSame<void>;
  };

  /**
   * PALs expose a basic library of memory operations.
   */
  template<typename PAL>
  concept ConceptPAL_memops = requires(void* vp, std::size_t sz)
  {
    { PAL::notify_not_using(vp, sz) } noexcept -> ConceptSame<void>;

    { PAL::template notify_using<NoZero>(vp, sz) } noexcept
      -> ConceptSame<void>;
    { PAL::template notify_using<YesZero>(vp, sz) } noexcept
      -> ConceptSame<void>;

    { PAL::template zero<false>(vp, sz) } noexcept -> ConceptSame<void>;
    { PAL::template zero<true>(vp, sz) } noexcept -> ConceptSame<void>;
  };

  /**
   * Absent any feature flags, the PAL must support a crude primitive allocator
   */
  template<typename PAL>
  concept ConceptPAL_reserve =
    requires(PAL p, std::size_t sz)
  {
    { PAL::reserve(sz) } noexcept -> ConceptSame<void*>;
  };

  /**
   * Some PALs expose a richer allocator which understands aligned allocations
   */
  template<typename PAL>
  concept ConceptPAL_reserve_aligned = requires(std::size_t sz)
  {
    { PAL::template reserve_aligned<true>(sz) } noexcept -> ConceptSame<void*>;
    { PAL::template reserve_aligned<false>(sz) } noexcept
      -> ConceptSame<void*>;
  };

  /**
   * Some PALs can provide memory pressure callbacks.
   */
  template<typename PAL>
  concept ConceptPAL_mem_low_notify = requires(PalNotificationObject* pno)
  {
    { PAL::expensive_low_memory_check() } -> ConceptSame<bool>;
    { PAL::register_for_low_memory_callback(pno) } -> ConceptSame<void>;
  };

  template<typename PAL>
  concept ConceptPAL_get_entropy64 = requires()
  {
    { PAL::get_entropy64() } -> ConceptSame<uint64_t>;
  };

  /**
   * PALs ascribe to the conjunction of several concepts.  These are broken
   * out by the shape of the requires() quantifiers required and by any
   * requisite claimed pal_features.  PALs not claiming particular features
   * are, naturally, not bound by the corresponding concept.
   */
  template<typename PAL>
  concept ConceptPAL =
    ConceptPAL_static_members<PAL> &&
    ConceptPAL_error<PAL> &&
    ConceptPAL_memops<PAL> &&
    (!pal_supports<Entropy, PAL> ||
      ConceptPAL_get_entropy64<PAL>) &&
    (!pal_supports<LowMemoryNotification, PAL> ||
      ConceptPAL_mem_low_notify<PAL>) &&
    (pal_supports<NoAllocation, PAL> ||
     (
       (!pal_supports<AlignedAllocation, PAL> ||
         ConceptPAL_reserve_aligned<PAL>) &&
      ConceptPAL_reserve<PAL>)
    );

} // namespace snmalloc
#endif
