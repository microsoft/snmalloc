#pragma once

#include "../mem/mem.h"

namespace snmalloc
{
  // Forward reference to thread local cleanup.
  void register_clean_up();

  /**
   * Options for a specific snmalloc configuration.  Every globals object must
   * have one `constexpr` instance of this class called `Options`.  This should
   * be constructed to explicitly override any of the defaults.  A
   * configuration that does not need to override anything would simply declare
   * this as a field of the global object:
   *
   * ```c++
   * constexpr static snmalloc::Flags Options{};
   * ```
   *
   * A global configuration that wished to use out-of-line message queues but
   * accept the defaults for everything else would instead do this:
   *
   * ```c++
   *     constexpr static snmalloc::Flags Options{.IsQueueInline = false};
   * ```
   *
   * To maintain backwards source compatibility in future versions, any new
   * option added here should have its default set to be whatever snmalloc was
   * doing before the new option was added.
   */
  struct Flags
  {
    /**
     * Should allocators have inline message queues?  If this is true then
     * the `CoreAllocator` is responsible for allocating the
     * `RemoteAllocator` that contains its message queue.  If this is false
     * then the `RemoteAllocator` must be separately allocated and provided
     * to the `CoreAllocator` before it is used.
     *
     * Setting this to `false` currently requires also setting
     * `LocalAllocSupportsLazyInit` to false so that the `CoreAllocator` can
     * be provided to the `LocalAllocator` fully initialised but in the
     * future it may be possible to allocate the `RemoteAllocator` via
     * `alloc_meta_data` or a similar API in the back end.
     */
    bool IsQueueInline = true;

    /**
     * Does the `CoreAllocator` own a `Backend::LocalState` object?  If this is
     * true then the `CoreAllocator` is responsible for allocating and
     * deallocating a local state object, otherwise the surrounding code is
     * responsible for creating it.
     *
     * Use cases that set this to false will probably also need to set
     * `LocalAllocSupportsLazyInit` to false so that they can provide the local
     * state explicitly during allocator creation.
     */
    bool CoreAllocOwnsLocalState = true;

    /**
     * Are `CoreAllocator` allocated by the pool allocator?  If not then the
     * code embedding this snmalloc configuration is responsible for allocating
     * `CoreAllocator` instances.
     *
     * Users setting this flag must also set `LocalAllocSupportsLazyInit` to
     * false currently because there is no alternative mechanism for allocating
     * core allocators.  This may change in future versions.
     */
    bool CoreAllocIsPoolAllocated = true;

    /**
     * Do `LocalAllocator` instances in this configuration support lazy
     * initialisation?  If so, then the first exit from a fast path will
     * trigger allocation of a `CoreAllocator` and associated state.  If not
     * then the code embedding this configuration of snmalloc is responsible
     * for allocating core allocators.
     */
    bool LocalAllocSupportsLazyInit = true;

    /**
     * Are the front and back pointers to the message queue in a RemoteAllocator
     * considered to be capptr_bounds::Wildness::Tame (as opposed to Wild)?
     * That is, is it presumed that clients or other potentialadversaries cannot
     * access the front and back pointers themselves, even if they can access
     * the queue nodes themselves (which are always considered Wild)?
     */
    bool QueueHeadsAreTame = true;
  };

  /**
   * Class containing definitions that are likely to be used by all except for
   * the most unusual back-end implementations.  This can be subclassed as a
   * convenience for back-end implementers, but is not required.
   */
  class CommonConfig
  {
  public:
    /**
     * Special remote that should never be used as a real remote.
     * This is used to initialise allocators that should always hit the
     * remote path for deallocation. Hence moving a branch off the critical
     * path.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    inline static RemoteAllocator unused_remote;
  };

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

} // namespace snmalloc
#include "../mem/remotecache.h"
