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
     * SFINAE helper to detect the presence of capptr_domesticate function in
     * backend. Returns true if there is a function with correct name and type.
     */
    template<
      SNMALLOC_CONCEPT(ConceptBackendDomestication) Backend,
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    constexpr SNMALLOC_FAST_PATH_INLINE auto has_domesticate(int)
      -> std::enable_if_t<
        std::is_same_v<
          decltype(Backend::capptr_domesticate(
            std::declval<typename Backend::LocalState*>(),
            std::declval<CapPtr<T, B>>())),
          CapPtr<
            T,
            typename B::template with_wildness<
              capptr::dimension::Wildness::Tame>>>,
        bool>
    {
      return true;
    }

    /**
     * SFINAE helper to detect the presence of capptr_domesticate function in
     * backend. Returns false in case where above template does not match.
     */
    template<
      SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend,
      typename T,
      SNMALLOC_CONCEPT(capptr::ConceptBound) B>
    constexpr SNMALLOC_FAST_PATH_INLINE bool has_domesticate(long)
    {
      return false;
    }
  } // namespace detail

  /**
   * Wrapper that calls `Backend::capptr_domesticate` if and only if
   * Backend::Options.HasDomesticate is true. If it is not implemented then
   * this assumes that any wild pointer can be domesticated.
   */
  template<
    SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend,
    typename T,
    SNMALLOC_CONCEPT(capptr::ConceptBound) B>
  SNMALLOC_FAST_PATH_INLINE auto
  capptr_domesticate(typename Backend::LocalState* ls, CapPtr<T, B> p)
  {
    static_assert(
      !detail::has_domesticate<Backend, T, B>(0) ||
        Backend::Options.HasDomesticate,
      "Back end provides domesticate function but opts out of using it ");

    static_assert(
      detail::has_domesticate<Backend, T, B>(0) ||
        !Backend::Options.HasDomesticate,
      "Back end does not provide capptr_domesticate and requests its use");
    if constexpr (Backend::Options.HasDomesticate)
    {
      return Backend::capptr_domesticate(ls, p);
    }
    else
    {
      UNUSED(ls);
      return CapPtr<
        T,
        typename B::template with_wildness<capptr::dimension::Wildness::Tame>>(
        p.unsafe_ptr());
    }
  }

  namespace detail
  {
    /**
     * SFINAE helper to detect the presence of reversion_alloc function in
     * backend. Returns true if there is a function with correct name and type.
     */
    template<SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend>
    constexpr SNMALLOC_FAST_PATH_INLINE auto has_reversion(int)
      -> std::enable_if_t<
        std::is_same_v<
          decltype(Backend::reversion_alloc(
            std::declval<typename capptr::Alloc<void>>(),
            std::declval<const typename Backend::Pagemap::Entry&>())),
          capptr::Alloc<void>>,
        bool>
    {
      return true;
    }

    /**
     * SFINAE helper to detect the presence of reversion_alloc function in
     * backend. Returns false in case where above template does not match.
     */
    template<SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend>
    constexpr SNMALLOC_FAST_PATH_INLINE bool has_reversion(long)
    {
      return false;
    }
  } // namespace detail

  /**
   * Wrapper that calls `Backend::reversion_alloc` if and only if
   * Backend::Options.HasReversion is true. If it is not implemented then
   * this provides a no-op version.
   */
  template<SNMALLOC_CONCEPT(ConceptBackendGlobals) Backend>
  SNMALLOC_FAST_PATH_INLINE auto reversion_alloc(
    capptr::Alloc<void> p_tame, const typename Backend::Pagemap::Entry& entry)
  {
    static_assert(
      !detail::has_reversion<Backend>(0) || Backend::Options.HasReversion,
      "Back end provides reversion function but opts out of using it ");
    static_assert(
      detail::has_reversion<Backend>(0) || !Backend::Options.HasReversion,
      "Back end does not provide reversion_alloc and requests its use");
    if constexpr (Backend::Options.HasReversion)
    {
      return Backend::reversion_alloc(p_tame, entry);
    }
    else
    {
      UNUSED(entry);
#ifdef SNMALLOC_TINTS_ZERO
      size_t len = sizeclass_full_to_size(entry.get_sizeclass());
      Backend::Pal::zero(p_tame.unsafe_ptr(), len);
#endif
      return p_tame;
    }
  }
} // namespace snmalloc
