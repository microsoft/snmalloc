#pragma once

#ifdef __cpp_concepts
#  include <cstddef>
#  include "../ds/concept.h"

namespace snmalloc
{
  class MetaEntry;

  /**
   * The core of the static pagemap accessor interface: {get,set}_metadata.
   *
   * get_metadata takes a bool-ean template parameter indicating whether it may
   * be accessing memory that is not known to be committed.
   */
  template<typename Meta>
  concept ConceptBackendMeta =
    requires(address_t addr, size_t sz, MetaEntry t)
  {
    { Meta::set_metaentry(addr, sz, t) } -> ConceptSame<void>;

    { Meta::template get_metaentry<true>(addr) }
      -> ConceptSame<const MetaEntry&>;

    { Meta::template get_metaentry<false>(addr) }
      -> ConceptSame<const MetaEntry&>;
  };

  /**
   * The pagemap can also be told to commit backing storage for a range of
   * addresses.  This is broken out to a separate concept so that we can
   * annotate which functions expect to do this vs. which merely use the core
   * interface above.  In practice, use ConceptBackendMetaRange (without the
   * underscore) below, which combines this and the core concept, above.
   */
  template<typename Meta>
  concept ConceptBackendMeta_Range =
    requires(address_t addr, size_t sz)
  {
    { Meta::register_range(addr, sz) } -> ConceptSame<void>;
  };

  /**
   * The full pagemap accessor interface, with all of {get,set}_metadata and
   * register_range.  Use this to annotate callers that need the full interface
   * and use ConceptBackendMeta for callers that merely need {get,set}_metadata,
   * but note that the difference is just for humans and not compilers (since
   * concept checking is lower bounding and does not constrain the templatized
   * code to use only those affordances given by the concept).
   */
  template<typename Meta>
  concept ConceptBackendMetaRange =
    ConceptBackendMeta<Meta> && ConceptBackendMeta_Range<Meta>;
} // namespace snmalloc

#endif
