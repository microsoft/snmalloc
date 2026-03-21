#pragma once

#include "../ds/allocconfig.h"
#include "../ds_core/ds_core.h"

namespace snmalloc
{
  /**
   * Pure-math helpers for the bitmap-indexed coalescing range.
   *
   * Provides the mapping from (block_size_chunks, block_align_chunks) to a
   * flat bitmap bin index, and the allocation lookup tables (start bit, mask
   * bit) for each size class.
   *
   * Size classes follow S = 2^e + m * 2^{e-B} where B = INTERMEDIATE_BITS.
   * The bitmap has PREFIX_BITS prefix entries (for sizes 1..2^B-1 in chunks)
   * plus SLOTS_PER_EXPONENT entries per exponent level e in [2, MAX_EXPONENT].
   *
   * The slot layout within each exponent group is:
   *   [A-only, B-only, both, +m2, +m3]
   * where A-only and B-only form the sole incomparable pair.  This structure
   * is specific to B=2 and guarded by static_asserts labelled [A1]-[A7].
   *
   * Templated on MAX_SIZE_BITS (passed from the range wrapper).
   */
  template<size_t MAX_SIZE_BITS>
  struct BitmapCoalesceHelpers
  {
    // ---- B=2 structural constants ----

    static constexpr size_t B = INTERMEDIATE_BITS;
    static constexpr size_t SL_COUNT = size_t(1) << B;

    /**
     * [A1] Slot count per exponent depends on the incomparable-pair structure
     * of the servable-set DAG, which is determined by alignment-tier count
     * (B+1 tiers).  For B=2: 1 incomparable pair -> 5 slots.
     */
    static_assert(
      INTERMEDIATE_BITS == 2, "[A1] SLOTS_PER_EXPONENT=5 assumes B=2");
    static constexpr size_t SLOTS_PER_EXPONENT = 5;

    /**
     * [A2] Prefix bins cover sizes 1..2^B-1 (the sub-exponent range).
     * For B=2 these are {1,2,3}.
     */
    static_assert(INTERMEDIATE_BITS == 2, "[A2] PREFIX_BITS=3 assumes B=2");
    static constexpr size_t PREFIX_BITS = 3;

    /**
     * Highest exponent in chunk units.  Exponent range is [2, MAX_EXPONENT],
     * giving MAX_EXPONENT - 1 levels of SLOTS_PER_EXPONENT each.
     */
    static constexpr size_t MAX_EXPONENT = MAX_SIZE_BITS - MIN_CHUNK_BITS;

    static constexpr size_t NUM_BINS =
      PREFIX_BITS + SLOTS_PER_EXPONENT * (MAX_EXPONENT - 1);

    static constexpr size_t BITMAP_WORDS =
      (NUM_BINS + bits::BITS - 1) / bits::BITS;

    // ---- Slot offsets within an exponent group ----
    // These name the 5 positions: A-only=0, B-only=1, both=2, +m2=3, +m3=4

    static constexpr size_t SLOT_A_ONLY = 0;
    static constexpr size_t SLOT_B_ONLY = 1;
    static constexpr size_t SLOT_BOTH = 2;
    static constexpr size_t SLOT_M2 = 3;
    static constexpr size_t SLOT_M3 = 4;

    // ---- Helpers ----

    /**
     * Natural alignment of a positive integer: largest power of 2 dividing it.
     * For 0, returns a very large power of 2.
     */
    static constexpr size_t natural_alignment(size_t s)
    {
      if (s == 0)
        return size_t(1) << (bits::BITS - 1);
      return s & ~(s - 1);
    }

    /**
     * Bit position of the first slot for exponent e (e >= 2).
     */
    static constexpr size_t exponent_base_bit(size_t e)
    {
      SNMALLOC_ASSERT(e >= 2);
      return PREFIX_BITS + (e - 2) * SLOTS_PER_EXPONENT;
    }

    /**
     * Decompose a valid size class (in chunks) into (exponent, mantissa).
     * Returns true on success, false if not a valid size class.
     *
     * For sizes 1..SL_COUNT-1 (prefix range): e <= 1, special handling.
     * For sizes >= SL_COUNT: standard S = 2^e + m * 2^{e-B}.
     */
    static constexpr bool decompose(size_t s, size_t& e_out, size_t& m_out)
    {
      if (s == 0)
        return false;
      if (s == 1)
      {
        e_out = 0;
        m_out = 0;
        return true;
      }
      if (s == 2)
      {
        e_out = 1;
        m_out = 0;
        return true;
      }
      if (s == 3)
      {
        e_out = 1;
        m_out = 1;
        return true;
      }

      // s >= 4: find fl = floor(log2(s))
      size_t fl = bits::BITS - 1 - bits::clz(s);
      if (fl < B)
        return false;
      size_t base = size_t(1) << fl;
      size_t step = base >> B;
      if (step == 0)
        return false;
      size_t remainder = s - base;
      if (remainder % step != 0)
        return false;
      size_t m = remainder / step;
      if (m >= SL_COUNT)
        return false;

      e_out = fl;
      m_out = m;
      return true;
    }

    /**
     * Size (in chunks) for a given (exponent, mantissa) pair.
     */
    static constexpr size_t sizeclass_size(size_t e, size_t m)
    {
      if (e == 0)
        return 1;
      if (e == 1)
        return m == 0 ? 2 : 3; // m can be 0 or 1 for e=1
      size_t base = size_t(1) << e;
      size_t step = base >> B;
      return base + m * step;
    }

    /**
     * Natural alignment of size class (e, m) in chunk units.
     */
    static constexpr size_t sizeclass_alignment(size_t e, size_t m)
    {
      return natural_alignment(sizeclass_size(e, m));
    }

    // ---- Threshold computation ----

    /**
     * Minimum block size (in chunks) needed to serve size class S at
     * block alignment alpha (in chunks).
     *
     * T(S, alpha) = S + max(0, align(S) - alpha)
     *
     * The block must be big enough for S itself plus any padding needed
     * to reach the first naturally-aligned address within the block.
     */
    static constexpr size_t threshold(size_t s, size_t alpha)
    {
      size_t a = natural_alignment(s);
      if (a <= alpha)
        return s;
      return s + a - alpha;
    }

    // ---- bin_index: the core mapping ----

    /**
     * [A7] Two threshold breakpoints per exponent assumes B+1 = 3 alignment
     * tiers producing exactly 1 incomparable pair.  Larger B -> more tiers ->
     * more breakpoints per exponent.
     *
     * For each exponent e, comparing against the size classes:
     *   m=0: S0 = 2^e,          align0 = 2^e
     *   m=1: S1 = 5*2^{e-2},    align1 = 2^{e-2}
     *   m=2: S2 = 3*2^{e-1},    align2 = 2^{e-1}
     *   m=3: S3 = 7*2^{e-2},    align3 = 2^{e-2}
     *
     *   T(S0, alpha) = 2^e + max(0, 2^e - alpha)
     *   T(S1, alpha) = 5*2^{e-2} + max(0, 2^{e-2} - alpha)
     *   T(S2, alpha) = 3*2^{e-1} + max(0, 2^{e-1} - alpha)
     *   T(S3, alpha) = 7*2^{e-2} + max(0, 2^{e-2} - alpha)
     *
     * The progression within an exponent at a given alpha is:
     *   A-only:  can serve S0 but not S1  (S0's threshold met, S1's not)
     *   B-only:  can serve S1 but not S0  (S1's threshold met, S0's not)
     *   both:    can serve S0 and S1, but not S2
     *   +m2:     can also serve S2, but not S3
     *   +m3:     can serve all four
     *
     * Whether A or B threshold is smaller depends on alpha:
     * - When alpha >= 2^e: T(S0) = 2^e, T(S1) = 5*2^{e-2} > T(S0), so A first
     * - When alpha < 2^{e-2}: T(S0) = 2^{e+1}-alpha, T(S1) = 3*2^{e-2}-alpha,
     *   so B first
     *
     * The bin_index function finds the highest bin the block qualifies for.
     */
    static_assert(
      INTERMEDIATE_BITS == 2, "[A7] bin_index threshold logic assumes B=2");

    static constexpr size_t bin_index(size_t n_chunks, size_t alpha_chunks)
    {
      if (n_chunks == 0)
        return 0;

      // Prefix range: sizes 1, 2, 3
      // Size 3: align(3) = 1, T(3, α) = 3 for all α.
      // Size 2: align(2) = 2, T(2, α) = 2 + max(0, 2 - α).
      // Size 1: align(1) = 1, always servable.
      if (n_chunks < SL_COUNT)
      {
        if (n_chunks >= 3)
          return 2;
        if (n_chunks >= threshold(2, alpha_chunks))
          return 1;
        return 0;
      }

      // For each exponent from high to low, check which slot the block
      // qualifies for.  We want the HIGHEST qualifying bin.
      //
      // Walk exponents downward.  At each level, check up to 4 size classes.
      // The thresholds within an exponent follow the slot progression:
      // +m3 (highest) >= +m2 >= both >= {A-only, B-only} (lowest pair).
      //
      // Since A-only and B-only are incomparable, we must check both.

      size_t best_bin = 0; // fallback: bin 0 (serves sizeclass 1)

      // Upper bound on exponent: n_chunks can't serve classes larger than
      // itself (even with perfect alignment).
      size_t max_e = bits::BITS - 1 - bits::clz(n_chunks);
      if (max_e > MAX_EXPONENT)
        max_e = MAX_EXPONENT;
      if (max_e < 2)
      {
        // Only prefix range is reachable
        if (n_chunks >= 3)
          return 2;
        return n_chunks - 1;
      }

      for (size_t e = max_e; e >= 2; e--)
      {
        size_t base_bit = exponent_base_bit(e);

        // Size classes at this exponent:
        size_t s0 = size_t(1) << e; // m=0
        size_t s1 = 5 * (size_t(1) << (e - 2)); // m=1
        size_t s2 = 3 * (size_t(1) << (e - 1)); // m=2
        size_t s3 = 7 * (size_t(1) << (e - 2)); // m=3

        // Thresholds:
        size_t t0 = threshold(s0, alpha_chunks);
        size_t t1 = threshold(s1, alpha_chunks);
        size_t t2 = threshold(s2, alpha_chunks);
        size_t t3 = threshold(s3, alpha_chunks);

        // Check from highest slot down
        if (n_chunks >= t3)
        {
          // Can serve all four mantissas at this exponent
          size_t candidate = base_bit + SLOT_M3;
          if (candidate >= best_bin)
            best_bin = candidate;
          // This is the highest slot at this exponent; no need to check lower.
          // But we also want the highest across ALL exponents, so we return
          // immediately since higher exponents have higher bit indices and
          // we're walking downward.
          return candidate;
        }
        if (n_chunks >= t2)
        {
          size_t candidate = base_bit + SLOT_M2;
          if (candidate >= best_bin)
            best_bin = candidate;
          return candidate;
        }

        // both: serves m=0 AND m=1 but not m=2
        bool can_m0 = n_chunks >= t0;
        bool can_m1 = n_chunks >= t1;

        if (can_m0 && can_m1)
        {
          size_t candidate = base_bit + SLOT_BOTH;
          if (candidate >= best_bin)
            best_bin = candidate;
          return candidate;
        }

        // A-only or B-only: incomparable pair
        if (can_m0)
        {
          size_t candidate = base_bit + SLOT_A_ONLY;
          if (candidate > best_bin)
            best_bin = candidate;
          // Don't return: lower exponent might have a higher total bin
          // (e.g. if we can serve all 4 mantissas at e-1, that's slot
          // base_bit(e-1)+4 which could be > base_bit(e)+0).
          // Actually: base_bit(e)+0 = PREFIX_BITS + (e-2)*5,
          // base_bit(e-1)+4 = PREFIX_BITS + (e-3)*5 + 4 = PREFIX_BITS + (e-2)*5
          // - 1 So A-only at e is always > any slot at e-1.  Return.
          return candidate;
        }
        if (can_m1)
        {
          size_t candidate = base_bit + SLOT_B_ONLY;
          if (candidate > best_bin)
            best_bin = candidate;
          // B-only at e = base_bit(e)+1 = PREFIX_BITS + (e-2)*5 + 1
          // Highest at e-1 = base_bit(e-1)+4 = PREFIX_BITS + (e-3)*5 + 4
          //                 = PREFIX_BITS + (e-2)*5 - 1
          // So B-only at e (offset 1) > highest at e-1 (offset -1).  Return.
          return candidate;
        }

        // Can't serve any size class at this exponent; try lower.
      }

      // Fall back to prefix range (same threshold logic as the early return).
      // For n >= SL_COUNT (=4), T(3, α) = 3 and T(2, α) ≤ 3 are always met,
      // so this always returns 2.  But keep the threshold checks for clarity.
      if (n_chunks >= 3)
        best_bin = 2;
      else if (n_chunks >= threshold(2, alpha_chunks))
        best_bin = bits::max(best_bin, size_t(1));
      // n_chunks >= 1 is always true (we checked n_chunks == 0 above)

      return best_bin;
    }

    // ---- Allocation lookup ----

    /**
     * [A6] The named slot layout {A-only, B-only, both, +m2, +m3} is the
     * specific DAG linearisation for B=2.
     *
     * [A3] Returns a single bit index for the mask (not a multi-bit mask).
     * For B>2, multiple incomparable slots could require a multi-bit mask.
     */
    static_assert(INTERMEDIATE_BITS == 2, "[A3,A6] alloc lookup assumes B=2");

    /**
     * Starting bit position for bitmap search when allocating size class
     * with exponent e and mantissa m.
     *
     * For prefix-range (e <= 1):
     *   sizeclass 1 -> bit 0, sizeclass 2 -> bit 1, sizeclass 3 -> bit 2
     *
     * For exponent-range (e >= 2):
     *   m=0: A-only bit of exponent e
     *   m=1: B-only bit of exponent e
     *   m=2: +m2 bit of exponent e
     *   m=3: +m3 bit of exponent e
     */
    static constexpr size_t alloc_start_bit(size_t e, size_t m)
    {
      if (e <= 1)
      {
        // Prefix range
        return sizeclass_size(e, m) - 1;
      }

      size_t base = exponent_base_bit(e);
      switch (m)
      {
        case 0:
          return base + SLOT_A_ONLY;
        case 1:
          return base + SLOT_B_ONLY;
        case 2:
          return base + SLOT_M2;
        case 3:
          return base + SLOT_M3;
        default:
          SNMALLOC_ASSERT(false);
          return 0;
      }
    }

    /**
     * Bit position to mask out when allocating m=0 at exponent e.
     * Returns SIZE_MAX if no masking is needed.
     *
     * [A4,A5] For B=2, only m=0 needs masking (mask out B-only),
     * and only 1 bit.
     */
    static constexpr size_t alloc_mask_bit(size_t e)
    {
      static_assert(INTERMEDIATE_BITS == 2, "[A4,A5] mask bit assumes B=2");

      if (e <= 1)
        return SIZE_MAX; // prefix range: no masking needed

      return exponent_base_bit(e) + SLOT_B_ONLY;
    }

    /**
     * Check if a chunk count is a valid size class.
     */
    static constexpr bool is_valid_sizeclass(size_t s)
    {
      if (s == 0)
        return false;
      size_t e, m;
      return decompose(s, e, m);
    }

    /**
     * Round up a chunk count to the next valid sizeclass.
     * Returns 0 for input 0.
     */
    static constexpr size_t round_up_sizeclass(size_t n)
    {
      if (n <= SL_COUNT)
        return n; // 0..4 are all valid (0 → 0, 1..4 → themselves)

      size_t fl = bits::BITS - 1 - bits::clz(n);
      size_t base = size_t(1) << fl;
      if (n == base)
        return n;

      size_t step = base >> B;
      size_t remainder = n - base;
      size_t m = (remainder + step - 1) / step; // round up
      if (m >= SL_COUNT)
        return size_t(1) << (fl + 1);

      return base + m * step;
    }
  };
} // namespace snmalloc
