#pragma once

#ifdef __cpp_concepts
#  include "../ds/concept.h"
#  include "../ds/ptrwrap.h"
#  include "aal_consts.h"

#  include <cstdint>
#  include <utility>

namespace snmalloc
{
  /**
   * AALs must advertise the bit vector of supported features, their name,
   * machine word size, and an upper bound on the address space size
   */
  template<typename AAL>
  concept ConceptAAL_static_members = requires()
  {
    typename std::integral_constant<uint64_t, AAL::aal_features>;
    typename std::integral_constant<int, AAL::aal_name>;
    typename std::integral_constant<std::size_t, AAL::bits>;
    typename std::integral_constant<std::size_t, AAL::address_bits>;
  };

  /**
   * AALs provide a prefetch operation.
   */
  template<typename AAL>
  concept ConceptAAL_prefetch = requires(void *ptr)
  {
    { AAL::prefetch(ptr) } noexcept -> ConceptSame<void>;
  };

  /**
   * AALs provide a notion of high-precision timing.
   */
  template<typename AAL>
  concept ConceptAAL_tick = requires()
  {
    { AAL::tick() } noexcept -> ConceptSame<uint64_t>;
  };

  template<typename AAL>
  concept ConceptAAL_capptr_methods =
  requires(CapPtr<void, CBArena> auth, CapPtr<void, CBAlloc> ret, size_t sz)
  {
    /**
     * Produce a pointer with reduced authority from a more privilged pointer.
     * The resulting pointer will have base at auth's address and length of
     * exactly sz.  auth+sz must not exceed auth's limit.
     */
    { AAL::template capptr_bound<void, CBChunk>(auth, sz) } noexcept
      -> ConceptSame<CapPtr<void, CBChunk>>;

    /**
     * Construct a copy of auth with its target set to that of ret.
     */
    { AAL::capptr_rebound(auth, ret) } noexcept
      -> ConceptSame<CapPtr<void, CBArena>>;
  };

  template<typename AAL>
  concept ConceptAAL =
    ConceptAAL_static_members<AAL> &&
    ConceptAAL_prefetch<AAL> &&
    ConceptAAL_tick<AAL> &&
    ConceptAAL_capptr_methods<AAL>;

} // namespace snmalloc
#endif
