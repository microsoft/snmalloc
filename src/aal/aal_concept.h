#pragma once

#ifdef __cpp_concepts
#  include "../ds/concept.h"
#  include "aal_consts.h"

#  include <cstdint>
#  include <utility>

namespace snmalloc
{
  /**
   * AALs must advertise the bit vector of supported features, their name,
   * 
   */
  template<typename AAL>
  concept ConceptAAL_static_members = requires()
  {
    typename std::integral_constant<uint64_t, AAL::aal_features>;
    typename std::integral_constant<int, AAL::aal_name>;
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
  concept ConceptAAL =
    ConceptAAL_static_members<AAL> &&
    ConceptAAL_prefetch<AAL> &&
    ConceptAAL_tick<AAL>;

} // namespace snmalloc
#endif
