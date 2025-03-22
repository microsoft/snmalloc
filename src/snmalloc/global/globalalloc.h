#pragma once

#include "../mem/mem.h"
#include "threadalloc.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  inline static void cleanup_unused()
  {
    static_assert(
      Config_::Options.AllocIsPoolAllocated,
      "Global cleanup is available only for pool-allocated configurations");
    // Call this periodically to free and coalesce memory allocated by
    // allocators that are not currently in use by any thread.
    // One atomic operation to extract the stack, another to restore it.
    // Handling the message queue for each stack is non-atomic.
    auto* first = AllocPool<Config_>::extract();
    auto* alloc = first;
    decltype(alloc) last;

    if (alloc != nullptr)
    {
      while (alloc != nullptr)
      {
        alloc->flush();
        last = alloc;
        alloc = AllocPool<Config_>::extract(alloc);
      }

      AllocPool<Config_>::restore(first, last);
    }
  }

  /**
    If you pass a pointer to a bool, then it returns whether all the
    allocators are empty. If you don't pass a pointer to a bool, then will
    raise an error all the allocators are not empty.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  inline static void debug_check_empty(bool* result = nullptr)
  {
    static_assert(
      Config_::Options.AllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    // This is a debugging function. It checks that all memory from all
    // allocators has been freed.
    auto* alloc = AllocPool<Config_>::iterate();

#ifdef SNMALLOC_TRACING
    message<1024>("debug check empty: first {}", alloc);
#endif
    bool done = false;
    bool okay = true;

    while (!done)
    {
#ifdef SNMALLOC_TRACING
      message<1024>("debug_check_empty: Check all allocators!");
#endif
      done = true;
      alloc = AllocPool<Config_>::iterate();
      okay = true;

      while (alloc != nullptr)
      {
#ifdef SNMALLOC_TRACING
        message<1024>("debug check empty: {}", alloc);
#endif
        // Check that the allocator has freed all memory.
        // repeat the loop if empty caused message sends.
        if (alloc->debug_is_empty(&okay))
        {
          done = false;
#ifdef SNMALLOC_TRACING
          message<1024>("debug check empty: sent messages {}", alloc);
#endif
        }

#ifdef SNMALLOC_TRACING
        message<1024>("debug check empty: okay = {}", okay);
#endif
        alloc = AllocPool<Config_>::iterate(alloc);
      }
    }

    // Check why this doesn't hold.
    // if (result == nullptr)
    //   SNMALLOC_CHECK(
    //     RemoteDeallocCache<Config_>::remote_inflight.get_curr() == 0);

    if (result != nullptr)
    {
      *result = okay;
      return;
    }

    // Redo check so abort is on allocator with allocation left.
    if (!okay)
    {
      alloc = AllocPool<Config_>::iterate();
      while (alloc != nullptr)
      {
        alloc->debug_is_empty(nullptr);
        alloc = AllocPool<Config_>::iterate(alloc);
      }
    }
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  inline static void debug_in_use(size_t count)
  {
    static_assert(
      Config_::Options.AllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    auto alloc = AllocPool<Config_>::iterate();
    while (alloc != nullptr)
    {
      if (alloc->debug_is_in_use())
      {
        if (count == 0)
        {
          error("ERROR: allocator in use.");
        }
        count--;
      }
      alloc = AllocPool<Config_>::iterate(alloc);

      if (count != 0)
      {
        error("Error: two few allocators in use.");
      }
    }
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  inline static void get_stats(AllocStats& stats)
  {
    auto alloc = AllocPool<Config>::iterate();
    while (alloc != nullptr)
    {
      stats += alloc->get_stats();
      alloc = AllocPool<Config>::iterate(alloc);
    }
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  inline static void print_alloc_stats()
  {
    static stl::Atomic<size_t> dump{0};

    auto l_dump = dump++;
    if (l_dump == 0)
    {
      message<1024>(
        "snmalloc_allocs,dumpid,sizeclass,size,allocated,deallocated,in_use,"
        "bytes,slabs allocated,slabs deallocated,slabs in_use,slabs bytes");
      message<1024>(
        "snmalloc_totals,dumpid,backend bytes,peak backend "
        "bytes,requested,slabs requested bytes,remote inflight bytes,allocator "
        "count");
    }

    AllocStats stats;
    snmalloc::get_stats<Config>(stats);
    size_t total_live{0};
    size_t total_live_slabs{0};
    for (size_t i = 0; i < snmalloc::SIZECLASS_REP_SIZE; i++)
    {
      auto sc = snmalloc::sizeclass_t::from_raw(i);
      auto allocated = *stats[sc].objects_allocated;
      auto deallocated = *stats[sc].objects_deallocated;
      auto slabs_allocated = *stats[sc].slabs_allocated;
      auto slabs_deallocated = *stats[sc].slabs_deallocated;
      if (allocated == 0 && deallocated == 0)
        continue;
      auto size = snmalloc::sizeclass_full_to_size(sc);
      auto slab_size = snmalloc::sizeclass_full_to_slab_size(sc);
      auto in_use = allocated - deallocated;
      auto amount = in_use * size;
      total_live += amount;
      auto in_use_slabs = slabs_allocated - slabs_deallocated;
      auto amount_slabs = in_use_slabs * slab_size;
      total_live_slabs += amount_slabs;

      snmalloc::message<1024>(
        "snmalloc_allocs,{},{},{},{},{},{},{},{},{},{},{}",
        l_dump,
        i,
        size,
        allocated,
        deallocated,
        in_use,
        amount,
        slabs_allocated,
        slabs_deallocated,
        in_use_slabs,
        amount_slabs);
    }
    snmalloc::message<1024>(
      "snmalloc_totals,{},{},{},{},{},{},{}",
      l_dump,
      Config::Backend::get_current_usage(),
      Config::Backend::get_peak_usage(),
      total_live,
      total_live_slabs,
      RemoteDeallocCache<Config>::remote_inflight.get_curr(),
      Config::pool().get_count());
  }

  /**
   * Returns the number of remaining bytes in an object.
   *
   * auto p = (char*)malloc(size)
   * remaining_bytes(p + n) == size - n     provided n < size
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  size_t SNMALLOC_FAST_PATH_INLINE remaining_bytes(address_t p)
  {
    const auto& entry = Config_::Backend::template get_metaentry<true>(p);

    auto sizeclass = entry.get_sizeclass();
    return snmalloc::remaining_bytes(sizeclass, p);
  }

  /**
   * Returns the byte offset into an object.
   *
   * auto p = (char*)malloc(size)
   * index_in_object(p + n) == n     provided n < size
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  static inline size_t index_in_object(address_t p)
  {
    const auto& entry = Config_::Backend::template get_metaentry<true>(p);

    auto sizeclass = entry.get_sizeclass();
    return snmalloc::index_in_object(sizeclass, p);
  }

  enum Boundary
  {
    /**
     * The location of the first byte of this allocation.
     */
    Start,
    /**
     * The location of the last byte of the allocation.
     */
    End,
    /**
     * The location one past the end of the allocation.  This is mostly useful
     * for bounds checking, where anything less than this value is safe.
     */
    OnePastEnd
  };

  /**
   * Returns the Start/End of an object allocated by this allocator
   *
   * It is valid to pass any pointer, if the object was not allocated
   * by this allocator, then it give the start and end as the whole of
   * the potential pointer space.
   */
  template<
    Boundary location = Start,
    SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  inline static void* external_pointer(void* p)
  {
    /*
     * Note that:
     * * each case uses `pointer_offset`, so that on CHERI, our behaviour is
     *   monotone with respect to the capability `p`.
     *
     * * the returned pointer could be outside the CHERI bounds of `p`, and
     *   thus not something that can be followed.
     *
     * * we don't use capptr_from_client()/capptr_reveal(), to avoid the
     *   syntactic clutter.  By inspection, `p` flows only to address_cast
     *   and pointer_offset, and so there's no risk that we follow or act
     *   to amplify the rights carried by `p`.
     */
    if constexpr (location == Start)
    {
      size_t index = index_in_object<Config_>(address_cast(p));
      return pointer_offset(p, 0 - index);
    }
    else if constexpr (location == End)
    {
      return pointer_offset(p, remaining_bytes(address_cast(p)) - 1);
    }
    else
    {
      return pointer_offset(p, remaining_bytes(address_cast(p)));
    }
  }

  /**
   * @brief Get the client meta data for the snmalloc allocation covering this
   * pointer.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  typename Config_::ClientMeta::DataRef get_client_meta_data(void* p)
  {
    const auto& entry = Config_::Backend::get_metaentry(address_cast(p));

    size_t index = slab_index(entry.get_sizeclass(), address_cast(p));

    auto* meta_slab = entry.get_slab_metadata();

    if (SNMALLOC_UNLIKELY(entry.is_backend_owned()))
    {
      error("Cannot access meta-data for write for freed memory!");
    }

    if (SNMALLOC_UNLIKELY(meta_slab == nullptr))
    {
      error(
        "Cannot access meta-data for non-snmalloc object in writable form!");
    }

    return meta_slab->get_meta_for_object(index);
  }

  /**
   * @brief Get the client meta data for the snmalloc allocation covering this
   * pointer.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  stl::add_const_t<typename Config_::ClientMeta::DataRef>
  get_client_meta_data_const(void* p)
  {
    const auto& entry =
      Config_::Backend::template get_metaentry<true>(address_cast(p));

    size_t index = slab_index(entry.get_sizeclass(), address_cast(p));

    auto* meta_slab = entry.get_slab_metadata();

    if (SNMALLOC_UNLIKELY((meta_slab == nullptr) || (entry.is_backend_owned())))
    {
      static typename Config_::ClientMeta::StorageType null_meta_store{};
      return Config_::ClientMeta::get(&null_meta_store, 0);
    }

    return meta_slab->get_meta_for_object(index);
  }

  /**
   * @brief Checks that the supplied size of the allocation matches the size
   * snmalloc believes the allocation is.  Only performs the check if
   *     mitigations(sanity_checks)
   * is enabled.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  SNMALLOC_FAST_PATH_INLINE void check_size(void* p, size_t size)
  {
    if constexpr (mitigations(sanity_checks))
    {
      const auto& entry = Config_::Backend::get_metaentry(address_cast(p));
      if (!entry.is_owned())
        return;
      size = size == 0 ? 1 : size;
      auto sc = size_to_sizeclass_full(size);
      auto pm_sc = entry.get_sizeclass();
      auto rsize = sizeclass_full_to_size(sc);
      auto pm_size = sizeclass_full_to_size(pm_sc);
      snmalloc_check_client(
        mitigations(sanity_checks),
        (sc == pm_sc) || (p == nullptr),
        "Dealloc rounded size mismatch: {} != {}",
        rsize,
        pm_size);
    }
    else
      UNUSED(p, size);
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  SNMALLOC_FAST_PATH_INLINE size_t alloc_size(const void* p_raw)
  {
    const auto& entry = Config_::Backend::get_metaentry(address_cast(p_raw));

    if (SNMALLOC_UNLIKELY(
          !Config::SecondaryAllocator::pass_through && !entry.is_owned() &&
          p_raw != nullptr))
      return Config::SecondaryAllocator::alloc_size(p_raw);
    // TODO What's the domestication policy here?  At the moment we just
    // probe the pagemap with the raw address, without checks.  There could
    // be implicit domestication through the `Config::Pagemap` or
    // we could just leave well enough alone.

    // Note that alloc_size should return 0 for nullptr.
    // Other than nullptr, we know the system will be initialised as it must
    // be called with something we have already allocated.
    //
    // To handle this case we require the uninitialised pagemap contain an
    // entry for the first chunk of memory, that states it represents a
    // large object, so we can pull the check for null off the fast path.

    return sizeclass_full_to_size(entry.get_sizeclass());
  }

  template<size_t size, typename Conts = Uninit, size_t align = 1>
  SNMALLOC_FAST_PATH_INLINE void* alloc()
  {
    return ThreadAlloc::get().alloc<Conts, ThreadAlloc::CheckInit>(
      aligned_size(align, size));
  }

  template<typename Conts = Uninit, size_t align = 1>
  SNMALLOC_FAST_PATH_INLINE void* alloc(size_t size)
  {
    return ThreadAlloc::get().alloc<Conts, ThreadAlloc::CheckInit>(
      aligned_size(align, size));
  }

  template<typename Conts = Uninit>
  SNMALLOC_FAST_PATH_INLINE void* alloc_aligned(size_t align, size_t size)
  {
    return ThreadAlloc::get().alloc<Conts, ThreadAlloc::CheckInit>(
      aligned_size(align, size));
  }

  SNMALLOC_FAST_PATH_INLINE void dealloc(void* p)
  {
    ThreadAlloc::get().dealloc<ThreadAlloc::CheckInit>(p);
  }

  SNMALLOC_FAST_PATH_INLINE void dealloc(void* p, size_t size)
  {
    check_size(p, size);
    ThreadAlloc::get().dealloc<ThreadAlloc::CheckInit>(p);
  }

  template<size_t size>
  SNMALLOC_FAST_PATH_INLINE void dealloc(void* p)
  {
    check_size(p, size);
    ThreadAlloc::get().dealloc<ThreadAlloc::CheckInit>(p);
  }

  SNMALLOC_FAST_PATH_INLINE void dealloc(void* p, size_t size, size_t align)
  {
    auto rsize = aligned_size(align, size);
    check_size(p, rsize);
    ThreadAlloc::get().dealloc<ThreadAlloc::CheckInit>(p);
  }

  SNMALLOC_FAST_PATH_INLINE void debug_teardown()
  {
    return ThreadAlloc::teardown();
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config_ = Config>
  SNMALLOC_FAST_PATH_INLINE bool is_owned(void* p)
  {
    const auto& entry = Config_::Backend::get_metaentry(address_cast(p));
    return entry.is_owned();
  }
} // namespace snmalloc
