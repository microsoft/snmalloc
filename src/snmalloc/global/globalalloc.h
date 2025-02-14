#pragma once

#include "../mem/mem.h"

namespace snmalloc
{
  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  inline static void cleanup_unused()
  {
    static_assert(
      Config::Options.CoreAllocIsPoolAllocated,
      "Global cleanup is available only for pool-allocated configurations");
    // Call this periodically to free and coalesce memory allocated by
    // allocators that are not currently in use by any thread.
    // One atomic operation to extract the stack, another to restore it.
    // Handling the message queue for each stack is non-atomic.
    auto* first = AllocPool<Config>::extract();
    auto* alloc = first;
    decltype(alloc) last;

    if (alloc != nullptr)
    {
      while (alloc != nullptr)
      {
        alloc->flush();
        last = alloc;
        alloc = AllocPool<Config>::extract(alloc);
      }

      AllocPool<Config>::restore(first, last);
    }
  }

  /**
    If you pass a pointer to a bool, then it returns whether all the
    allocators are empty. If you don't pass a pointer to a bool, then will
    raise an error all the allocators are not empty.
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  inline static void debug_check_empty(bool* result = nullptr)
  {
    static_assert(
      Config::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    // This is a debugging function. It checks that all memory from all
    // allocators has been freed.
    auto* alloc = AllocPool<Config>::iterate();

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
      alloc = AllocPool<Config>::iterate();
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
        alloc = AllocPool<Config>::iterate(alloc);
      }
    }

    if (result != nullptr)
    {
      *result = okay;
      return;
    }

    // Redo check so abort is on allocator with allocation left.
    if (!okay)
    {
      alloc = AllocPool<Config>::iterate();
      while (alloc != nullptr)
      {
        alloc->debug_is_empty(nullptr);
        alloc = AllocPool<Config>::iterate(alloc);
      }
    }
  }

  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  inline static void debug_in_use(size_t count)
  {
    static_assert(
      Config::Options.CoreAllocIsPoolAllocated,
      "Global status is available only for pool-allocated configurations");
    auto alloc = AllocPool<Config>::iterate();
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
      alloc = AllocPool<Config>::iterate(alloc);

      if (count != 0)
      {
        error("Error: two few allocators in use.");
      }
    }
  }

  /**
   * Returns the number of remaining bytes in an object.
   *
   * auto p = (char*)malloc(size)
   * remaining_bytes(p + n) == size - n     provided n < size
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  size_t remaining_bytes(address_t p)
  {
    const auto& entry = Config::Backend::template get_metaentry<true>(p);

    auto sizeclass = entry.get_sizeclass();
    return snmalloc::remaining_bytes(sizeclass, p);
  }

  /**
   * Returns the byte offset into an object.
   *
   * auto p = (char*)malloc(size)
   * index_in_object(p + n) == n     provided n < size
   */
  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  static inline size_t index_in_object(address_t p)
  {
    const auto& entry = Config::Backend::template get_metaentry<true>(p);

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
    SNMALLOC_CONCEPT(IsConfig) Config = Config>
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
      size_t index = index_in_object<Config>(address_cast(p));
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
  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  typename Config::ClientMeta::DataRef get_client_meta_data(void* p)
  {
    const auto& entry = Config::Backend::get_metaentry(address_cast(p));

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
  template<SNMALLOC_CONCEPT(IsConfig) Config = Config>
  stl::add_const_t<typename Config::ClientMeta::DataRef>
  get_client_meta_data_const(void* p)
  {
    const auto& entry =
      Config::Backend::template get_metaentry<true>(address_cast(p));

    size_t index = slab_index(entry.get_sizeclass(), address_cast(p));

    auto* meta_slab = entry.get_slab_metadata();

    if (SNMALLOC_UNLIKELY((meta_slab == nullptr) || (entry.is_backend_owned())))
    {
      static typename Config::ClientMeta::StorageType null_meta_store{};
      return Config::ClientMeta::get(&null_meta_store, 0);
    }

    return meta_slab->get_meta_for_object(index);
  }
} // namespace snmalloc
