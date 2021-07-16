#pragma once

#include "../ds/bits.h"
#include "../ds/helpers.h"
#include "../pal/pal.h"

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
      std::cout << "Pagemap.init " << b << " (" << s << ")" << std::endl;
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
      body = reinterpret_cast<T*>(b);

      // Advance by size of pagemap.
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
      static constexpr size_t COVERED_BITS =
        bits::ADDRESS_BITS - GRANULARITY_BITS;
      static constexpr size_t ENTRIES = bits::one_at_bit(COVERED_BITS);

      // TODO request additional space, and move to random offset.

      // TODO wasting space if size2 bigger than needed.
      auto [new_body_untyped, size2] =
        Pal::reserve_at_least(ENTRIES * sizeof(T));

      auto new_body = reinterpret_cast<T*>(new_body_untyped);

      // Ensure bottom page is committed
      // ASSUME: new memory is zeroed.
      Pal::notify_using<NoZero>(new_body, OS_PAGE_SIZE);

      // Set up zero page
      new_body[0] = body[0];

      body = new_body;
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
      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          if constexpr (potentially_out_of_range)
          {
            return default_value;
          }
          else
          {
            // Out of range null should
            // still return the default value.
            if (p == 0)
              return default_value;
            PAL::error("Internal error: Pagemap read access out of range.");
          }
        }
        p = p - base;
      }

      //  This means external pointer on Windows will be slow.
      if constexpr (potentially_out_of_range && !pal_supports<LazyCommit, PAL>)
      {
        register_range(p, 1);
      }

      return body[p >> SHIFT];
    }

    void set(address_t p, T t)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "Pagemap.Set " << (void*)p << std::endl;
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
} // namespace snmalloc
