#pragma once

#include "../ds/helpers.h"
#include "superslab.h"

namespace snmalloc
{
  struct SizeClassTable
  {
    ModArray<NUM_SIZECLASSES, size_t> size;
    ModArray<NUM_SIZECLASSES, size_t> cache_friendly_mask;
    ModArray<NUM_SIZECLASSES, size_t> inverse_cache_friendly_mask;
    ModArray<NUM_SMALL_CLASSES, uint16_t> bump_ptr_start;
    ModArray<NUM_SMALL_CLASSES, uint16_t> short_bump_ptr_start;
    ModArray<NUM_SMALL_CLASSES, uint16_t> count_per_slab;
    ModArray<NUM_MEDIUM_CLASSES, uint16_t> medium_slab_slots;

    constexpr SizeClassTable()
    : size(),
      cache_friendly_mask(),
      inverse_cache_friendly_mask(),
      bump_ptr_start(),
      short_bump_ptr_start(),
      count_per_slab(),
      medium_slab_slots()
    {
      for (uint8_t sizeclass = 0; sizeclass < NUM_SIZECLASSES; sizeclass++)
      {
        size[sizeclass] =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);

        size_t alignment = bits::min(
          (size_t)1 << bits::ctz_const(size[sizeclass]), OS_PAGE_SIZE);
        cache_friendly_mask[sizeclass] = (alignment - 1);
        inverse_cache_friendly_mask[sizeclass] = ~(alignment - 1);
      }

      size_t header_size = sizeof(Superslab);
      size_t short_slab_size = SLAB_SIZE - header_size;

      for (uint8_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        short_bump_ptr_start[i] =
          (uint16_t)(1 + (short_slab_size % size[i]) + header_size);
        bump_ptr_start[i] = (uint16_t)(1 + (SLAB_SIZE % size[i]));
        count_per_slab[i] = (uint16_t)(SLAB_SIZE / size[i]);
      }

      for (uint8_t i = NUM_SMALL_CLASSES; i < NUM_SIZECLASSES; i++)
      {
        medium_slab_slots[i - NUM_SMALL_CLASSES] =
          (uint16_t)((SUPERSLAB_SIZE - Mediumslab::header_size()) / size[i]);
      }
    }
  };

  static constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  static inline constexpr uint16_t get_slab_offset(uint8_t sc, bool is_short)
  {
    if (is_short)
      return sizeclass_metadata.short_bump_ptr_start[sc];
    else
      return sizeclass_metadata.bump_ptr_start[sc];
  }

  constexpr static inline size_t sizeclass_to_size(uint8_t sizeclass)
  {
    return sizeclass_metadata.size[sizeclass];
  }

  constexpr static inline size_t
  sizeclass_to_cache_friendly_mask(uint8_t sizeclass)
  {
    return sizeclass_metadata.cache_friendly_mask[sizeclass];
  }

  constexpr static inline size_t
  sizeclass_to_inverse_cache_friendly_mask(uint8_t sizeclass)
  {
    return sizeclass_metadata.inverse_cache_friendly_mask[sizeclass];
  }

  constexpr static inline size_t sizeclass_to_count(uint8_t sizeclass)
  {
    return sizeclass_metadata.count_per_slab[sizeclass];
  }

  constexpr static inline uint16_t medium_slab_free(uint8_t sizeclass)
  {
    return sizeclass_metadata
      .medium_slab_slots[(sizeclass - NUM_SMALL_CLASSES)];
  }
}
