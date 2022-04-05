#pragma once
/**
 * Several of the functions provided by the back end are optional.  This file
 * contains helpers that are templated on a back end and either call the
 * corresponding function or do nothing.  This allows the rest of the front end
 * to assume that these functions always exist and avoid the need for `if
 * constexpr` clauses everywhere.  The no-op versions are always inlined and so
 * will be optimised away.
 */

#include "../ds_core/ds_core.h"

namespace snmalloc
{
  /**
   * SFINAE helper.  Matched only if `T` implements `is_initialised`.  Calls
   * it if it exists.
   */
  template<typename T>
  SNMALLOC_FAST_PATH auto call_is_initialised(T*, int)
    -> decltype(T::is_initialised())
  {
    return T::is_initialised();
  }

  /**
   * SFINAE helper.  Matched only if `T` does not implement `is_initialised`.
   * Unconditionally returns true if invoked.
   */
  template<typename T>
  SNMALLOC_FAST_PATH auto call_is_initialised(T*, long)
  {
    return true;
  }

  namespace detail
  {
    /**
     * SFINAE helper, calls capptr_domesticate in the backend if it exists.
     */
    template<
      SNMALLOC_CONCEPT(ConceptBackendDomestication) Backend,
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    SNMALLOC_FAST_PATH_INLINE auto
    capptr_domesticate(typename Backend::LocalState* ls, CapPtr<T, B> p, int)
      -> decltype(Backend::capptr_domesticate(ls, p))
    {
      return Backend::capptr_domesticate(ls, p);
    }

    /**
     * SFINAE helper.  If the back end does not provide special handling for
     * domestication then assume all wild pointers can be domesticated.
     */
    template<
      SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend,
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    SNMALLOC_FAST_PATH_INLINE auto
    capptr_domesticate(typename Backend::LocalState*, CapPtr<T, B> p, long)
    {
      return CapPtr<
        T,
        typename B::template with_wildness<capptr::dimension::Wildness::Tame>>(
        p.unsafe_ptr());
    }
  } // namespace detail

  /**
   * Wrapper that calls `Backend::capptr_domesticate` if and only if it is
   * implemented.  If it is not implemented then this assumes that any wild
   * pointer can be domesticated.
   */
  template<
    SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend,
    typename T,
    SNMALLOC_CONCEPT(capptr::ConceptBound) B>
  SNMALLOC_FAST_PATH_INLINE auto
  capptr_domesticate(typename Backend::LocalState* ls, CapPtr<T, B> p)
  {
    return detail::capptr_domesticate<Backend>(ls, p, 0);
  }

}
