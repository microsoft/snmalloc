#pragma once

#ifdef __cpp_concepts
#  include <cstddef>
#  include "../ds/concept.h"
#  include "../pal/pal_concept.h"

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
    requires(
      typename Meta::LocalState* ls,
      address_t addr,
      size_t sz,
      MetaEntry t)
  {
    { Meta::set_metaentry(ls, addr, sz, t) } -> ConceptSame<void>;

    { Meta::template get_metaentry<true>(ls, addr) }
      -> ConceptSame<const MetaEntry&>;

    { Meta::template get_metaentry<false>(ls, addr) }
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
    requires(typename Meta::LocalState* ls, address_t addr, size_t sz)
  {
    { Meta::register_range(ls, addr, sz) } -> ConceptSame<void>;
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

  class CommonConfig;
  struct Flags;

  /**
   * Backend global objects of type T must obey a number of constraints.  They
   * must...
   *
   *  * inherit from CommonConfig (see commonconfig.h)
   *  * specify which PAL is in use via T::Pal
   *  * have static pagemap accessors via T::Pagemap
   *  * define a T::LocalState type (and alias it as T::Pagemap::LocalState)
   *  * define T::Options of type snmalloc::Flags
   *  * expose the global allocator pool via T::pool()
   *
   */
  template<typename Globals>
  concept ConceptBackendGlobals =
    std::is_base_of<CommonConfig, Globals>::value &&
    ConceptPAL<typename Globals::Pal> &&
    ConceptBackendMetaRange<typename Globals::Pagemap> &&
    ConceptSame<
      typename Globals::LocalState,
      typename Globals::Pagemap::LocalState> &&
    requires()
    {
      typename Globals::LocalState;

      { Globals::Options } -> ConceptSameModRef<const Flags>;

      typename Globals::GlobalPoolState;
      { Globals::pool() } -> ConceptSame<typename Globals::GlobalPoolState &>;
    };

  /**
   * The lazy version of the above; please see ds/concept.h and use sparingly.
   */
  template<typename Globals>
  concept ConceptBackendGlobalsLazy =
    !is_type_complete_v<Globals> || ConceptBackendGlobals<Globals>;

} // namespace snmalloc

#endif
