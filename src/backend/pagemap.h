#pragma once

#include "../ds/bits.h"
#include "../ds/helpers.h"
#include "../ds/invalidptr.h"

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
    bool has_bounds,
    T* default_value = nullptr>
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
    T* body{default_value};

    address_t base{0};
    size_t size{0};

  public:
    constexpr FlatPagemap() {}

    template<typename ASM>
    void init(ASM* a, address_t b = 0, size_t s = 0)
    {
      if constexpr (has_bounds)
      {
        SNMALLOC_ASSERT(s != 0);
        base = b;
        size = s;
        body = a->template reserve<false, false>((size >> SHIFT) * sizeof(T))
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

        // TODO Pal notify here
        // Pal::notify_using<NoZero>(new_body, sizeof(T));
        // Set up zero page
        new_body[0] = body[0];

        body = new_body;
        //        madvise(body, size, MADV_NOHUGEPAGE);
      }
    }

    /**
     * If the location has not been used before, then
     * `potentially_out_of_range` should be set to true.
     * This will ensure there is a location for the
     * read/write.
     */
    template<bool potentially_out_of_range>
    T get(address_t p)
    {
      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          if constexpr (potentially_out_of_range)
          {
            return *default_value;
          }
          else
          {
            // Out of range null should
            // still return the default value.
            if (p == 0)
              return *default_value;
            Pal::error("Internal error.");
          }
        }
        p = p - base;
      }

      //  This means external pointer on Windows will be slow.
      if constexpr (potentially_out_of_range)
      {
        // TODO: need to uncomment
        // Pal::notify_using<NoZero>(&body[p >> SHIFT], sizeof(T));
      }

      return body[p >> SHIFT];
    }

    void set(address_t p, T t)
    {
      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          Pal::error("Internal error.");
        }
        p = p - base;
      }

      body[p >> SHIFT] = t;
    }

    void add(address_t p, T t)
    {
      if constexpr (has_bounds)
      {
        if (p - base > size)
        {
          Pal::error("Internal error.");
        }
        p = p - base;
      }

      // This could be the first time this page is used
      // This will potentially be expensive on Windows,
      // and we should revisit the performance here.
      // TODO: need to uncomment
      //      Pal::notify_using<NoZero>(&body[p >> SHIFT], sizeof(T));

      body[p >> SHIFT] = t;
    }
  };
} // namespace snmalloc
