#pragma once

#include "../ds/ds.h"
#include "../mem/mem.h"

#include <atomic>
#include <utility>

namespace snmalloc
{
  /**
   * Simple pagemap that for each GRANULARITY_BITS of the address range
   * stores a T.
   */
  template<size_t GRANULARITY_BITS, typename T, typename PAL, bool has_bounds>
  class FlatPagemap
  {
  private:
    static constexpr size_t SHIFT = GRANULARITY_BITS;

    /**
     * Before init is called will contain a single entry
     * that is the default value.  This is needed so that
     * various calls do not have to check for nullptr.
     *   free(nullptr)
     * and
     *   malloc_usable_size(nullptr)
     * do not require an allocation to have ocurred before
     * they are called.
     */
    inline static const T default_value{};

    /**
     * The representation of the page map.
     *
     * Initially a single element to ensure nullptr operations
     * work.
     */
    T* body{const_cast<T*>(&default_value)};

    /**
     * The representation of the pagemap, but nullptr if it has not been
     * initialised.  Used to combine init checking and lookup.
     */
    T* body_opt{nullptr};

    /**
     * If `has_bounds` is set, then these should contain the
     * bounds of the heap that is being managed by this pagemap.
     */
    address_t base{0};
    size_t size{0};

  public:
    /**
     * Ensure this range of pagemap is accessible
     */
    void register_range(address_t p, size_t length)
    {
      // Calculate range in pagemap that is associated to this space.
      auto first = &body[p >> SHIFT];
      auto last = &body[(p + length + bits::one_at_bit(SHIFT) - 1) >> SHIFT];

      // Commit OS pages associated to the range.
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(first);
      auto page_end = pointer_align_up<OS_PAGE_SIZE, char>(last);
      size_t using_size = pointer_diff(page_start, page_end);
      PAL::template notify_using<NoZero>(page_start, using_size);
    }

    constexpr FlatPagemap() = default;

    /**
     * For pagemaps that cover an entire fixed address space, return the size
     * that they must be.  This allows the caller to allocate the correct
     * amount of memory to be passed to `init`.  This is not available for
     * fixed-range pagemaps, whose size depends on dynamic configuration.
     */
    template<bool has_bounds_ = has_bounds>
    static constexpr std::enable_if_t<!has_bounds_, size_t> required_size()
    {
      static_assert(
        has_bounds_ == has_bounds, "Don't set SFINAE template parameter!");
      constexpr size_t COVERED_BITS = PAL::address_bits - GRANULARITY_BITS;
      constexpr size_t ENTRIES = bits::one_at_bit(COVERED_BITS);
      return ENTRIES * sizeof(T);
    }

    /**
     * Initialise with pre-allocated memory.
     *
     * This is currently disabled for bounded pagemaps but may be reenabled if
     * `required_size` is enabled for the has-bounds case.
     */
    template<bool has_bounds_ = has_bounds>
    std::enable_if_t<!has_bounds_> init(T* address)
    {
      static_assert(
        has_bounds_ == has_bounds, "Don't set SFINAE template parameter!");
      body = address;
      body_opt = address;
    }

    /**
     * Initialise the pagemap with bounds.
     *
     * Returns usable range after pagemap has been allocated
     */
    template<bool has_bounds_ = has_bounds>
    std::enable_if_t<has_bounds_, std::pair<void*, size_t>>
    init(void* b, size_t s)
    {
      static_assert(
        has_bounds_ == has_bounds, "Don't set SFINAE template parameter!");
#ifdef SNMALLOC_TRACING
      message<1024>("Pagemap.init {} ({})", b, s);
#endif
      SNMALLOC_ASSERT(s != 0);
      // TODO take account of pagemap size in the calculation of how big it
      // needs to be.

      // Align the start and end.  We won't store for the very ends as they
      // are not aligned to a chunk boundary.
      auto heap_base = pointer_align_up(b, bits::one_at_bit(GRANULARITY_BITS));
      auto end = pointer_align_down(
        pointer_offset(b, s), bits::one_at_bit(GRANULARITY_BITS));
      size = pointer_diff(heap_base, end);

      // Put pagemap at start of range.
      // TODO CHERI capability bound here!
      body = static_cast<T*>(b);
      body_opt = body;
      // Advance by size of pagemap.
      // Note that base needs to be aligned to GRANULARITY for the rest of the
      // code to work
      // TODO CHERI capability bound here!
      heap_base = pointer_align_up(
        pointer_offset(b, (size >> SHIFT) * sizeof(T)),
        bits::one_at_bit(GRANULARITY_BITS));
      base = address_cast(heap_base);
      SNMALLOC_ASSERT(
        base == bits::align_up(base, bits::one_at_bit(GRANULARITY_BITS)));

      return {heap_base, pointer_diff(heap_base, end)};
    }

    /**
     * Initialise the pagemap without bounds.
     */
    template<bool has_bounds_ = has_bounds>
    std::enable_if_t<!has_bounds_> init()
    {
      static_assert(
        has_bounds_ == has_bounds, "Don't set SFINAE template parameter!");
      static constexpr size_t REQUIRED_SIZE = required_size();

#ifdef SNMALLOC_CHECK_CLIENT
      // Allocate a power of two extra to allow the placement of the
      // pagemap be difficult to guess.
      size_t additional_size = bits::next_pow2(REQUIRED_SIZE) * 4;
      size_t request_size = REQUIRED_SIZE + additional_size;
#else
      size_t request_size = REQUIRED_SIZE;
#endif

      auto new_body_untyped = PAL::reserve(request_size);

      if (new_body_untyped == nullptr)
      {
        PAL::error("Failed to initialise snmalloc.");
      }

#ifdef SNMALLOC_CHECK_CLIENT
      // Begin pagemap at random offset within the additionally allocated space.
      static_assert(bits::is_pow2(sizeof(T)), "Next line assumes this.");
      size_t offset = get_entropy64<PAL>() & (additional_size - sizeof(T));
      auto new_body = pointer_offset<T>(new_body_untyped, offset);

      if constexpr (pal_supports<LazyCommit, PAL>)
      {
        void* start_page = pointer_align_down<OS_PAGE_SIZE>(new_body);
        void* end_page = pointer_align_up<OS_PAGE_SIZE>(
          pointer_offset(new_body, REQUIRED_SIZE));
        // Only commit readonly memory for this range, if the platform supports
        // lazy commit.  Otherwise, this would be a lot of memory to have
        // mapped.
        PAL::notify_using_readonly(
          start_page, pointer_diff(start_page, end_page));
      }
#else
      auto new_body = static_cast<T*>(new_body_untyped);
#endif
      // Ensure bottom page is committed
      // ASSUME: new memory is zeroed.
      PAL::template notify_using<NoZero>(
        pointer_align_down<OS_PAGE_SIZE>(new_body), OS_PAGE_SIZE);

      // Set up zero page
      new_body[0] = body[0];

      body = new_body;
      body_opt = new_body;
    }

    template<bool has_bounds_ = has_bounds>
    std::enable_if_t<has_bounds_, std::pair<address_t, size_t>> get_bounds()
    {
      static_assert(
        has_bounds_ == has_bounds, "Don't set SFINAE template parameter!");

      return {base, size};
    }

    /**
     * Get the number of entries.
     */
    [[nodiscard]] constexpr size_t num_entries() const
    {
      if constexpr (has_bounds)
      {
        return size >> GRANULARITY_BITS;
      }
      else
      {
        return bits::one_at_bit(PAL::address_bits - GRANULARITY_BITS);
      }
    }

    /**
     * If the location has not been used before, then
     * `potentially_out_of_range` should be set to true.
     * This will ensure there is a location for the
     * read/write.
     */
    template<bool potentially_out_of_range>
    T& get_mut(address_t p)
    {
      if constexpr (potentially_out_of_range)
      {
        if (SNMALLOC_UNLIKELY(body_opt == nullptr))
          return const_cast<T&>(default_value);
      }

      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          if constexpr (potentially_out_of_range)
          {
            return const_cast<T&>(default_value);
          }
          else
          {
            // Out of range null should
            // still return the default value.
            if (p == 0)
              return const_cast<T&>(default_value);
            PAL::error("Internal error: Pagemap read access out of range.");
          }
        }
        p = p - base;
      }

      //  If this is potentially_out_of_range, then the pages will not have
      //  been mapped. With Lazy commit they will at least be mapped read-only
      //  Note that: this means external pointer on Windows will be slow.
      if constexpr (potentially_out_of_range && !pal_supports<LazyCommit, PAL>)
      {
        register_range(p, 1);
      }

      if constexpr (potentially_out_of_range)
        return body_opt[p >> SHIFT];
      else
        return body[p >> SHIFT];
    }

    /**
     * If the location has not been used before, then
     * `potentially_out_of_range` should be set to true.
     * This will ensure there is a location for the
     * read/write.
     */
    template<bool potentially_out_of_range>
    const T& get(address_t p)
    {
      return get_mut<potentially_out_of_range>(p);
    }

    /**
     * Check if the pagemap has been initialised.
     */
    [[nodiscard]] bool is_initialised() const
    {
      return body_opt != nullptr;
    }

    /**
     * Return the starting address corresponding to a given entry within the
     * Pagemap. Also checks that the reference actually points to a valid entry.
     */
    [[nodiscard]] address_t get_address(const T& t) const
    {
      address_t entry_offset = address_cast(&t) - address_cast(body);
      address_t entry_index = entry_offset / sizeof(T);
      SNMALLOC_ASSERT(
        entry_offset % sizeof(T) == 0 && entry_index < num_entries());
      return base + (entry_index << GRANULARITY_BITS);
    }

    void set(address_t p, const T& t)
    {
#ifdef SNMALLOC_TRACING
      message<1024>("Pagemap.Set {}", p);
#endif
      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          PAL::error("Internal error: Pagemap write access out of range.");
        }
        p = p - base;
      }

      body[p >> SHIFT] = t;
    }
  };

  /**
   * This is a generic implementation of the backend's interface to the page
   * map. It takes a concrete page map implementation (probably FlatPagemap
   * above) and entry type. It is friends with the backend passed in as a
   * template parameter so that the backend can initialise the concrete page map
   * and use set_metaentry which no one else should use.
   */
  template<
    typename PAL,
    typename ConcreteMap,
    typename PagemapEntry,
    bool fixed_range>
  class BasicPagemap
  {
  public:
    /**
     * Export the type stored in the pagemap.
     */
    using Entry = PagemapEntry;

    /**
     * Instance of the concrete pagemap, accessible to the backend so that
     * it can call the init method whose type dependent on fixed_range.
     */
    SNMALLOC_REQUIRE_CONSTINIT
    static inline ConcreteMap concretePagemap;

    /**
     * Set the metadata associated with a chunk.
     */
    SNMALLOC_FAST_PATH
    static void set_metaentry(address_t p, size_t size, const Entry& t)
    {
      for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
      {
        concretePagemap.set(a, t);
      }
    }

    /**
     * Get the metadata associated with a chunk.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a chunk.
     */
    template<bool potentially_out_of_range = false>
    SNMALLOC_FAST_PATH static const auto& get_metaentry(address_t p)
    {
      return concretePagemap.template get<potentially_out_of_range>(p);
    }

    /**
     * Get the metadata associated with a chunk.
     *
     * Set template parameter to true if it not an error
     * to access a location that is not backed by a chunk.
     */
    template<bool potentially_out_of_range = false>
    SNMALLOC_FAST_PATH static auto& get_metaentry_mut(address_t p)
    {
      return concretePagemap.template get_mut<potentially_out_of_range>(p);
    }

    /**
     * Register a range in the pagemap as in-use, requiring it to allow writing
     * to the underlying memory.
     */
    static void register_range(address_t p, size_t sz)
    {
      concretePagemap.register_range(p, sz);
    }

    /**
     * Return the bounds of the memory this back-end manages as a pair of
     * addresses (start then end).  This is available iff this is a
     * fixed-range Backend.
     */
    template<bool fixed_range_ = fixed_range>
    static SNMALLOC_FAST_PATH
      std::enable_if_t<fixed_range_, std::pair<address_t, address_t>>
      get_bounds()
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      return concretePagemap.get_bounds();
    }

    /**
     * Return whether the pagemap is initialised, ready for access.
     */
    static bool is_initialised()
    {
      return concretePagemap.is_initialised();
    }
  };
} // namespace snmalloc
