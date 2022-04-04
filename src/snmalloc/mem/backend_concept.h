#pragma once

#ifdef __cpp_concepts
#  include "../ds/ds.h"

#  include <cstddef>
namespace snmalloc
{
  /**
   * The core of the static pagemap accessor interface: {get,set}_metadata.
   *
   * get_metadata takes a boolean template parameter indicating whether it may
   * be accessing memory that is not known to be committed.
   */
  template<typename Meta>
  concept ConceptBackendMeta =
    requires(address_t addr, size_t sz, const typename Meta::Entry& t)
  {
    {
      Meta::template get_metaentry<true>(addr)
    }
    ->ConceptSame<const typename Meta::Entry&>;

    {
      Meta::template get_metaentry<false>(addr)
    }
    ->ConceptSame<const typename Meta::Entry&>;
  };

  /**
   * The pagemap can also be told to commit backing storage for a range of
   * addresses.  This is broken out to a separate concept so that we can
   * annotate which functions expect to do this vs. which merely use the core
   * interface above.  In practice, use ConceptBackendMetaRange (without the
   * underscore) below, which combines this and the core concept, above.
   */
  template<typename Meta>
  concept ConceptBackendMeta_Range = requires(address_t addr, size_t sz)
  {
    {
      Meta::register_range(addr, sz)
    }
    ->ConceptSame<void>;
  };

  template<typename Meta>
  concept ConceptBuddyRangeMeta =
    requires(address_t addr, size_t sz, const typename Meta::Entry& t)
  {
    {
      Meta::template get_metaentry_mut<true>(addr)
    }
    ->ConceptSame<typename Meta::Entry&>;

    {
      Meta::template get_metaentry_mut<false>(addr)
    }
    ->ConceptSame<typename Meta::Entry&>;
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
    ConceptBackendMeta<Meta>&& ConceptBackendMeta_Range<Meta>;

  /**
   * The backend also defines domestication (that is, the difference between
   * Tame and Wild CapPtr bounds).  It exports the intended affordance for
   * testing a Wild pointer and either returning nullptr or the original
   * pointer, now Tame.
   */
  template<typename Globals>
  concept ConceptBackendDomestication =
    requires(typename Globals::LocalState* ls, capptr::AllocWild<void> ptr)
  {
    {
      Globals::capptr_domesticate(ls, ptr)
    }
    ->ConceptSame<capptr::Alloc<void>>;

    {
      Globals::capptr_domesticate(ls, ptr.template as_static<char>())
    }
    ->ConceptSame<capptr::Alloc<char>>;
  };

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
   *  * expose the global allocator pool via T::pool() if pool allocation is
   * used.
   *
   */
  template<typename Globals>
  concept ConceptBackendGlobals =
    std::is_base_of<CommonConfig, Globals>::value&&
      ConceptPAL<typename Globals::Pal>&&
        ConceptBackendMetaRange<typename Globals::Pagemap>&& requires()
  {
    typename Globals::LocalState;

    {
      Globals::Options
    }
    ->ConceptSameModRef<const Flags>;
  }
  &&(
    requires() {
      Globals::Options.CoreAllocIsPoolAllocated == true;
      typename Globals::GlobalPoolState;
      {
        Globals::pool()
      }
      ->ConceptSame<typename Globals::GlobalPoolState&>;
    } ||
    requires() { Globals::Options.CoreAllocIsPoolAllocated == false; });

  /**
   * The lazy version of the above; please see ds/concept.h and use sparingly.
   */
  template<typename Globals>
  concept ConceptBackendGlobalsLazy =
    !is_type_complete_v<Globals> || ConceptBackendGlobals<Globals>;

} // namespace snmalloc

#endif
