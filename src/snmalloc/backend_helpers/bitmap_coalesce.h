#pragma once

#include "../ds/ds.h"
#include "bitmap_coalesce_helpers.h"

namespace snmalloc
{
  /**
   * Bitmap-indexed coalescing allocator for free memory.
   *
   * Manages free memory blocks stored in a flat bitmap with one bit per
   * bin.  Bins are indexed by BitmapCoalesceHelpers::bin_index, which
   * maps (block_size_chunks, block_alignment_chunks) to a flat index
   * that encodes both the servable size classes and the block's alignment
   * tier.
   *
   * Blocks are stored at their actual size (no decomposition into
   * sizeclass blocks).  The allocation search finds
   * a block guaranteed to serve the requested sizeclass at *some*
   * naturally-aligned address within the block.  The range wrapper
   * (Step 4) carves the exact aligned allocation from the returned block.
   *
   * The Rep template provides pagemap access (see
   * BitmapCoalesceRep in bitmap_coalesce_range.h for the concept).
   *
   * Template parameters:
   *   Rep            - Pagemap accessor implementing the Rep concept.
   *   MAX_SIZE_BITS  - Maximum block size this range manages (log2 bytes).
   */
  template<typename Rep, size_t MAX_SIZE_BITS>
  class BitmapCoalesce
  {
    using BC = BitmapCoalesceHelpers<MAX_SIZE_BITS>;

    static constexpr size_t NUM_BINS = BC::NUM_BINS;
    static constexpr size_t BITMAP_WORDS = BC::BITMAP_WORDS;

    /**
     * Head pointers for each bin's singly-linked free list.
     * 0 means the bin is empty.
     */
    address_t bin_heads[NUM_BINS] = {};

    /**
     * Single flat bitmap: bit i is set iff bin_heads[i] is non-empty.
     */
    size_t bitmap[BITMAP_WORDS] = {};

    // ---- Bitmap operations ----

    void set_bit(size_t idx)
    {
      SNMALLOC_ASSERT(idx < NUM_BINS);
      bitmap[idx / bits::BITS] |= size_t(1) << (idx % bits::BITS);
    }

    void clear_bit(size_t idx)
    {
      SNMALLOC_ASSERT(idx < NUM_BINS);
      bitmap[idx / bits::BITS] &= ~(size_t(1) << (idx % bits::BITS));
    }

    [[nodiscard]] bool test_bit(size_t idx) const
    {
      SNMALLOC_ASSERT(idx < NUM_BINS);
      return (bitmap[idx / bits::BITS] >> (idx % bits::BITS)) & 1;
    }

    /**
     * Find the first set bit at position >= start, skipping mask_bit
     * if it is not SIZE_MAX.  Returns SIZE_MAX if no bit is found.
     */
    [[nodiscard]] size_t find_set_from(size_t start, size_t mask_bit) const
    {
      SNMALLOC_ASSERT(start < NUM_BINS);

      size_t word_idx = start / bits::BITS;
      size_t bit_ofs = start % bits::BITS;

      for (size_t w = word_idx; w < BITMAP_WORDS; w++)
      {
        size_t word = bitmap[w];

        // Apply mask if mask_bit falls in this word.
        if (mask_bit != SIZE_MAX)
        {
          size_t mask_word = mask_bit / bits::BITS;
          if (w == mask_word)
            word &= ~(size_t(1) << (mask_bit % bits::BITS));
        }

        // Clear bits below start in the first word.
        if (w == word_idx)
          word &= ~((size_t(1) << bit_ofs) - 1);

        if (word != 0)
        {
          size_t result = w * bits::BITS + bits::ctz(word);
          if (result < NUM_BINS)
            return result;
          return SIZE_MAX;
        }
      }
      return SIZE_MAX;
    }

    // ---- Chunk-level helpers ----

    /**
     * Chunk-level address alignment: natural alignment of the chunk index
     * at byte address addr.  For address 0, returns a very large alignment.
     */
    static size_t chunk_alignment(address_t addr)
    {
      auto chunk_addr = addr / MIN_CHUNK_SIZE;
      if (chunk_addr == 0)
        return size_t(1) << (bits::BITS - 2);
      return BC::natural_alignment(chunk_addr);
    }

    // ---- Bin operations ----

    /**
     * Insert a free block into its bin.  Prepend to the singly-linked
     * list, set boundary tags at both ends, mark coalesce_free, set bitmap bit.
     */
    void insert_block(address_t addr, size_t size)
    {
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(size % MIN_CHUNK_SIZE == 0);

      size_t n_chunks = size / MIN_CHUNK_SIZE;
      size_t alpha = chunk_alignment(addr);
      size_t bin = BC::bin_index(n_chunks, alpha);

      SNMALLOC_ASSERT(bin < NUM_BINS);

      Rep::set_boundary_tags(addr, size);
      Rep::set_next(addr, bin_heads[bin]);
      bin_heads[bin] = addr;

      Rep::set_coalesce_free(addr);
      if (size > MIN_CHUNK_SIZE)
      {
        Rep::set_coalesce_free(addr + size - MIN_CHUNK_SIZE);
      }

      set_bit(bin);
    }

    /**
     * Remove a specific block from its bin's linked list.
     * Returns true if found and removed, false otherwise.
     */
    bool remove_from_bin(address_t addr, size_t size)
    {
      size_t n_chunks = size / MIN_CHUNK_SIZE;
      size_t alpha = chunk_alignment(addr);
      size_t bin = BC::bin_index(n_chunks, alpha);

      address_t prev = 0;
      address_t curr = bin_heads[bin];

      while (curr != 0)
      {
        if (curr == addr)
        {
          auto next = Rep::get_next(curr);
          if (prev == 0)
            bin_heads[bin] = next;
          else
            Rep::set_next(prev, next);

          if (bin_heads[bin] == 0)
            clear_bit(bin);

          return true;
        }
        prev = curr;
        curr = Rep::get_next(curr);
      }

      return false;
    }

  public:
    constexpr BitmapCoalesce() = default;

    /**
     * Result of a block removal.
     */
    struct RemoveResult
    {
      /// Base address of the block, or 0 if not found.
      address_t addr;
      /// Size of the block in bytes, or 0 if not found.
      size_t size;
    };

    /**
     * Insert a block without coalescing.
     *
     * Inserts the block at its natural bin based on (size, alignment).
     * Aligns the input to MIN_CHUNK_SIZE boundaries.
     */
    void add_fresh_range(address_t addr, size_t length)
    {
      if (length == 0)
        return;

      auto aligned_start = bits::align_up(addr, MIN_CHUNK_SIZE);
      auto aligned_end = bits::align_down(addr + length, MIN_CHUNK_SIZE);
      if (aligned_end <= aligned_start)
        return;

      insert_block(aligned_start, aligned_end - aligned_start);
    }

    /**
     * Find and remove a block that can serve the given sizeclass.
     *
     * The size must be chunk-aligned and a valid sizeclass in chunk units.
     * Returns {addr, block_size} or {0, 0} if not found.
     *
     * The returned block may be larger than requested, and its address
     * may not be naturally aligned for the sizeclass.  The caller
     * (range wrapper) is responsible for carving.
     *
     * Clears the first-entry boundary tag and coalesce_free marker to
     * prevent stale reads by the coalescing left walk.
     */
    RemoveResult remove_block(size_t size)
    {
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(size % MIN_CHUNK_SIZE == 0);

      size_t n_chunks = size / MIN_CHUNK_SIZE;

      size_t e = 0, m = 0;
      bool valid = BC::decompose(n_chunks, e, m);
      SNMALLOC_ASSERT(valid);
      UNUSED(valid);

      size_t start = BC::alloc_start_bit(e, m);

      // [A4,A5] Apply mask for m=0 searches: skip the B-only bin at this
      // exponent.  For B=2, only m=0 needs masking and only 1 bit is masked.
      static_assert(
        INTERMEDIATE_BITS == 2,
        "[A4,A5] single-bit mask for m=0 only assumes B=2");
      size_t mask_bit = SIZE_MAX;
      if (m == 0 && e >= 2)
        mask_bit = BC::alloc_mask_bit(e);

      size_t bin = find_set_from(start, mask_bit);
      if (bin == SIZE_MAX)
        return {0, 0};

      // Pop head from this bin's list.
      address_t addr = bin_heads[bin];
      SNMALLOC_ASSERT(addr != 0);

      size_t block_size = Rep::get_size(addr);
      SNMALLOC_ASSERT(block_size >= MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(block_size % MIN_CHUNK_SIZE == 0);

      bin_heads[bin] = Rep::get_next(addr);
      if (bin_heads[bin] == 0)
        clear_bit(bin);

      // Clear the first-entry boundary tag and coalesce free marker to
      // prevent stale reads by the left walk in add_block.  The last
      // entry's boundary tag and marker are left stale: the left walk's
      // cross-check catches this.
      Rep::set_size(addr, 0);
      Rep::clear_coalesce_free(addr);

      return {addr, block_size};
    }

    /**
     * Add a block with coalescing.
     *
     * Merges with adjacent free blocks using left and right walks,
     * then inserts the coalesced block.
     *
     * (Implemented in Step 3.)
     */
    void add_block(address_t addr, size_t size)
    {
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);
      SNMALLOC_ASSERT(size % MIN_CHUNK_SIZE == 0);

      address_t merge_start = addr;
      address_t merge_end = addr + size;

      // Phase 1: Left walk.
      while (merge_start > 0 && !Rep::is_boundary(merge_start))
      {
        address_t prev_last = merge_start - MIN_CHUNK_SIZE;
        if (!Rep::is_free_block(prev_last))
          break;

        size_t prev_size = Rep::get_size(prev_last);
        if (prev_size == 0 || prev_size > merge_start)
          break;

        address_t prev_start = merge_start - prev_size;
        if (!Rep::is_free_block(prev_start))
          break;

        if (Rep::get_size(prev_start) != prev_size)
          break;

        bool removed = remove_from_bin(prev_start, prev_size);
        SNMALLOC_ASSERT(removed);
        UNUSED(removed);

        merge_start = prev_start;
      }

      // Phase 2: Right walk.
      while (!Rep::is_boundary(merge_end))
      {
        if (!Rep::is_free_block(merge_end))
          break;

        size_t next_size = Rep::get_size(merge_end);
        if (next_size == 0)
          break;

        bool removed = remove_from_bin(merge_end, next_size);
        SNMALLOC_ASSERT(removed);
        UNUSED(removed);

        // Clear absorbed block's tags to prevent stale reads by
        // subsequent left walks.
        Rep::set_size(merge_end, 0);
        if (next_size > MIN_CHUNK_SIZE)
          Rep::set_size(merge_end + next_size - MIN_CHUNK_SIZE, 0);

        merge_end += next_size;
      }

      // Insert the coalesced block.
      insert_block(merge_start, merge_end - merge_start);
    }

    // ---- Test/debug access ----

    /**
     * Check if the bitmap bit for a given bin is set.
     * Exposed for testing.
     */
    [[nodiscard]] bool is_bin_non_empty(size_t bin) const
    {
      SNMALLOC_ASSERT(bin < NUM_BINS);
      return test_bit(bin);
    }

    /**
     * Get the head of a bin's linked list.
     * Exposed for testing.
     */
    [[nodiscard]] address_t get_bin_head(size_t bin) const
    {
      SNMALLOC_ASSERT(bin < NUM_BINS);
      return bin_heads[bin];
    }

    /**
     * Get the number of bins.
     */
    static constexpr size_t num_bins()
    {
      return NUM_BINS;
    }
  };
} // namespace snmalloc
