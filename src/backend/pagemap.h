#pragma once

#include "../ds/bits.h"
#include "../ds/helpers.h"
#include "../ds/invalidptr.h"
#include "../pal/pal.h"

#include <atomic>
#include <utility>

namespace snmalloc
{
  /**
   * Simple pagemap that for each GRANULARITY_BITS of the address range
   * stores a T.
   */
  template<
    size_t GRANULARITY_BITS,
    typename T,
    typename PAL,
    bool has_bounds>
  class FlatPagemap
  {
  private:
    static constexpr size_t SHIFT = GRANULARITY_BITS;

    // Before init is called will contain a single entry
    // that is the default value.  This is needed so that
    // various calls do not have to check for nullptr.
    //   free(nullptr)
    // and
    //   malloc_usable_size(nullptr)
    // do not require an allocation to have ocurred before
    // they are called.
    inline static const T default_value{};
    T* body{const_cast<T*>(&default_value)};

    address_t base{0};
    size_t size{0};

    /**
     * Commit entry
     */
    void commit_entry(void* base)
    {
      auto entry_size = sizeof(T);
      static_assert(sizeof(T) < OS_PAGE_SIZE);
      // Rounding required for sub-page allocations.
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(base, entry_size));
      size_t using_size = pointer_diff(page_start, page_end);
      PAL::template notify_using<NoZero>(page_start, using_size);
    }

  public:
    constexpr FlatPagemap() {}

    template<typename ASM>
    void init(ASM* a, address_t b = 0, size_t s = 0)
    {
      if constexpr (has_bounds)
      {
#ifdef SNMALLOC_TRACING
        std::cout << "Pagemap.init " << (void*)b << " (" << s << ")"
                  << std::endl;
#endif
        SNMALLOC_ASSERT(s != 0);
        // Align the start and end.  We won't store for the very ends as they
        // are not aligned to a chunk boundary.
        base = bits::align_up(b, bits::one_at_bit(GRANULARITY_BITS));
        auto end = bits::align_down(b + s, bits::one_at_bit(GRANULARITY_BITS));
        size = end - base;
        body = a->template reserve<false, false>(bits::next_pow2((size >> SHIFT) * sizeof(T)))
                 .template as_static<T>()
                 .unsafe_capptr;
        ;
      }
      else
      {
        static constexpr size_t COVERED_BITS =
          bits::ADDRESS_BITS - GRANULARITY_BITS;
        static constexpr size_t ENTRIES = bits::one_at_bit(COVERED_BITS);
        auto new_body = (a->template reserve<false, false>(ENTRIES * sizeof(T)))
                          .template as_static<T>()
                          .unsafe_capptr;

        // Ensure bottom page is committed
        commit_entry(&new_body[0]);

        // Set up zero page
        new_body[0] = body[0];

        body = new_body;
        //TODO this is pretty sparse, should we ignore huge pages for it?
        //     madvise(body, size, MADV_NOHUGEPAGE);
      }
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
      if constexpr (potentially_out_of_range)
      {
        commit_entry(&body[p >> SHIFT]);
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

    void add(address_t p, T t)
    {
#ifdef SNMALLOC_TRACING
      std::cout << "Pagemap.Add " << (void*)p << std::endl;
#endif
      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          PAL::error("Internal error: Pagemap new write access out of range.");
        }
        p = p - base;
      }

      // This could be the first time this page is used
      // This will potentially be expensive on Windows,
      // and we should revisit the performance here.
      commit_entry(&body[p >> SHIFT]);

      body[p >> SHIFT] = t;
    }
  };
} // namespace snmalloc
