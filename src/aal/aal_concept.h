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

  /**
   * AALs provide the primitive operations for pointer authority manipulation,
   * including restriction and amplification.
   */
  template<typename AAL>
  concept ConceptAAL_ptrauth_members = requires()
  {
    /**
     * Specify the authority root granule size
     */
    typename std::integral_constant<size_t, AAL::ptrauth_root_alloc_size>;
  };

  template<typename AAL>
  concept ConceptAAL_ptrauth_methods =
  requires(AuthPtr<void> auth, ReturnPtr ret, size_t sz)
  {
    /**
     * Produce a pointer with reduced authority from a more privilged pointer.
     * The resulting pointer will have base at auth's address and length of
     * exactly sz.  auth+sz must not exceed auth's limit.
     */
    { AAL::template ptrauth_bound<void>(auth, sz) } noexcept -> ConceptSame<FreePtr<void>>;

    /**
     * Construct a copy of auth with its target set to that of ret.
     *
     * If auth is nullptr, must return nullptr in an AuthPtr wrapping,
     * regardless of ret; this implies not crashing on nullptr auth.
     */
    { AAL::ptrauth_rebound(auth, ret) } noexcept -> ConceptSame<AuthPtr<void>>;
  };

  template<typename AAL>
  concept ConceptAAL =
    ConceptAAL_static_members<AAL> &&
    ConceptAAL_prefetch<AAL> &&
    ConceptAAL_tick<AAL> &&
    ConceptAAL_ptrauth_methods<AAL> &&
    (!(AAL::aal_features & StrictProvenance) ||
      ConceptAAL_ptrauth_members<AAL>);

} // namespace snmalloc
#endif
