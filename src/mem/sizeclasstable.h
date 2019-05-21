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
    ModArray<NUM_SMALL_CLASSES, uint16_t> initial_link_ptr;
    ModArray<NUM_SMALL_CLASSES, uint16_t> short_initial_link_ptr;
    ModArray<NUM_MEDIUM_CLASSES, uint16_t> medium_slab_slots;

    constexpr SizeClassTable()
    : size(),
      cache_friendly_mask(),
      inverse_cache_friendly_mask(),
      bump_ptr_start(),
      short_bump_ptr_start(),
      initial_link_ptr(),
      short_initial_link_ptr(),
      medium_slab_slots()
    {
      for (uint8_t sizeclass = 0; sizeclass < NUM_SIZECLASSES; sizeclass++)
      {
        size[sizeclass] =
          bits::from_exp_mant<INTERMEDIATE_BITS, MIN_ALLOC_BITS>(sizeclass);

        size_t alignment = bits::min(
          bits::one_at_bit(bits::ctz_const(size[sizeclass])), OS_PAGE_SIZE);
        cache_friendly_mask[sizeclass] = (alignment - 1);
        inverse_cache_friendly_mask[sizeclass] = ~(alignment - 1);
      }

      size_t header_size = sizeof(Superslab);
      size_t short_slab_size = SLAB_SIZE - header_size;

      for (uint8_t i = 0; i < NUM_SMALL_CLASSES; i++)
      {
        // We align to the end of the block to remove special cases for the
        // short block. Calculate remainders
        size_t short_correction = short_slab_size % size[i];
        size_t correction = SLAB_SIZE % size[i];

        // First element in the block is the link
        initial_link_ptr[i] = static_cast<uint16_t>(correction);
        short_initial_link_ptr[i] =
          static_cast<uint16_t>(header_size + short_correction);

        // Move to object after link.
        auto short_after_link = short_initial_link_ptr[i] + size[i];
        size_t after_link = initial_link_ptr[i] + size[i];

        // Bump ptr has bottom bit set.
        // In case we only have one object on this slab check for wrap around.
        short_bump_ptr_start[i] =
          static_cast<uint16_t>((short_after_link + 1) % SLAB_SIZE);
        bump_ptr_start[i] = static_cast<uint16_t>((after_link + 1) % SLAB_SIZE);
      }

      for (uint8_t i = NUM_SMALL_CLASSES; i < NUM_SIZECLASSES; i++)
      {
        medium_slab_slots[i - NUM_SMALL_CLASSES] = static_cast<uint16_t>(
          (SUPERSLAB_SIZE - Mediumslab::header_size()) / size[i]);
      }
    }
  };

  static constexpr SizeClassTable sizeclass_metadata = SizeClassTable();

  static inline constexpr uint16_t
  get_initial_bumpptr(uint8_t sc, bool is_short)
  {
    if (is_short)
      return sizeclass_metadata.short_bump_ptr_start[sc];

    return sizeclass_metadata.bump_ptr_start[sc];
  }

  static inline constexpr uint16_t get_initial_link(uint8_t sc, bool is_short)
  {
    if (is_short)
      return sizeclass_metadata.short_initial_link_ptr[sc];

    return sizeclass_metadata.initial_link_ptr[sc];
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

  constexpr static inline uint16_t medium_slab_free(uint8_t sizeclass)
  {
    return sizeclass_metadata
      .medium_slab_slots[(sizeclass - NUM_SMALL_CLASSES)];
  }
} // namespace snmalloc
