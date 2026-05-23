#pragma once

#include "../ds_core/bits.h"
#include "../ds_core/helpers.h"

#include <stdint.h>

namespace snmalloc
{
  template<size_t INTERMEDIATE_BITS, size_t MIN_SIZE_BITS>
  struct ArenaBinsTestAccess;

  /**
   * Size class enumeration and bin classification used by the
   * Arena.
   *
   * Template parameter `B` (mantissa-bit width of snmalloc's
   * non-power-of-two size class scheme) determines the number of
   * RB-trees per exponent — the count of distinct servable subsets a
   * free block can occupy at that exponent: B=1 -> 2; B=2 -> 5;
   * B=3 -> 13. The canonical within-exponent bin numbering matches
   * `prototype/skip_analysis.py`. All bin-scheme metadata derives
   * constexpr from a single per-bin subsets table, `bin_subsets`.
   *
   * Template parameter `MIN_SIZE_BITS` is the log2 of the allocation
   * unit: every byte size handled here is a multiple of
   * `UNIT_SIZE = 1 << MIN_SIZE_BITS`, and the smallest representable
   * size is `UNIT_SIZE`. With `MIN_SIZE_BITS == 0` the unit is a single
   * byte and the classifier degenerates to the bare bin scheme;
   * larger values scale the entire size axis (and the bin tables)
   * by `UNIT_SIZE`.
   *
   * Public surface:
   *  - `range_t`, `carve_t`: byte ranges and carve output.
   *  - `carve(block, n)`: split a block into pre-pad / aligned
   *    request / post-pad, where `n` is in bytes.
   *  - `max_supported_size()`: upper bound on legal request sizes
   *    (in bytes).
   *  - nested `Bitmap`: per-arena non-empty-bins bitmap with
   *    `add` / `find_for_request` / `clear`.
   *
   * Everything else is private; tests reach it via
   * `ArenaBinsTestAccess<B, MIN_SIZE_BITS>`.
   */
  template<size_t INTERMEDIATE_BITS, size_t MIN_SIZE_BITS>
  class ArenaBins
  {
    static_assert(
      INTERMEDIATE_BITS >= 1 && INTERMEDIATE_BITS <= 3,
      "ArenaBins currently supports B in {1, 2, 3}");
    static_assert(
      MIN_SIZE_BITS + INTERMEDIATE_BITS < bits::BITS,
      "MIN_SIZE_BITS + INTERMEDIATE_BITS must leave room for at least one "
      "exponent above the low regime so MAX_SC is non-trivial");

  public:
    /// (base, size) byte range. Both fields are multiples of
    /// `UNIT_SIZE = 1 << MIN_SIZE_BITS`. `size == 0` means empty
    /// (base is unspecified).
    struct range_t
    {
      size_t base;
      size_t size;
    };

    /// Output of `carve`: pre-pad / aligned request / post-pad.
    /// Either or both of `pre`/`post` may be empty.
    struct carve_t
    {
      range_t pre;
      range_t req;
      range_t post;
    };

  private:
    friend struct ArenaBinsTestAccess<INTERMEDIATE_BITS, MIN_SIZE_BITS>;

    static constexpr size_t B = INTERMEDIATE_BITS;

    /// Size of the allocation unit. Every byte size handled by the
    /// classifier is a multiple of this value, and the smallest
    /// representable size is `UNIT_SIZE`.
    static constexpr size_t UNIT_SIZE = size_t(1) << MIN_SIZE_BITS;

    /// Number of mantissa positions per regular exponent (= 2^B).
    static constexpr size_t MANTISSAS_PER_EXP = size_t(1) << B;

    /// Number of distinct servable-subset bins per exponent
    /// (from prototype/skip_analysis.py).
    static constexpr size_t BINS_PER_EXP = (B == 1) ? 2 :
      (B == 2)                                      ? 5 :
      (B == 3)                                      ? 13 :
                                                      0;

    /// Size of the per-sc info tables. One past the largest raw id from
    /// `bits::to_exp_mant_const<B, MIN_SIZE_BITS>` whose decoded size
    /// fits in `size_t` (the architectural max raw id would decode to
    /// `2^bits::BITS`, which overflows).
    static constexpr size_t MAX_SC =
      ((bits::BITS - B - MIN_SIZE_BITS) << B) + ((size_t(1) << B) - 1);

    /**
     * Per-SC bitmap-scan record, read by `Bitmap::find_for_request`.
     * Fields are pre-shifted into the bitmap's word layout so the
     * search hot path is two ANDs.
     *
     *  - `start_word`: bitmap word containing this SC's start bin.
     *  - `first_mask`: serve mask pre-shifted into `start_word`. Bit
     *    `i` set iff `words_[start_word]` bit `i` serves this SC.
     *  - `second_mask`: serve mask carried into `start_word + 1`. When
     *    the start bin is word-aligned there is no within-exp carry
     *    and bits there are all higher-exponent, so `second_mask == ~0`.
     *
     * `alignas(4 * sizeof(size_t))` rounds `sizeof(bitmap_info_t)` up
     * to a power of two so `table_.bitmap_info[sc]` indexes with a
     * single shift+add.
     *
     * A *bin* (single bit in `Bitmap`) has no size/alignment of its
     * own; it may be set on behalf of any SC whose subset includes it.
     */
    struct alignas(4 * sizeof(size_t)) bitmap_info_t
    {
      size_t start_word;
      size_t first_mask;
      size_t second_mask;
    };

    static_assert(
      sizeof(bitmap_info_t) == 4 * sizeof(size_t),
      "bitmap_info_t must be 4*size_t so table_.bitmap_info[sc] indexes "
      "with a single shift+add; revisit the alignas if fields change");

    /**
     * Per-SC carve record, read by `carve` and by `bin_offset_at`'s
     * `fits` predicate (free-side cascade walk via `bin_index`).
     *
     *  - `size`: byte size this SC promises on allocation (multiple
     *    of `UNIT_SIZE`).
     *  - `align`: natural byte alignment (a power of two, derived
     *    from `size`).
     */
    struct carve_info_t
    {
      size_t size;
      size_t align;
    };

    static_assert(
      sizeof(carve_info_t) == 2 * sizeof(size_t),
      "carve_info_t must be 2*size_t so table_.carve_info[sc] indexes "
      "with a single shift+add");

    /**
     * Map a request size to its bitmap-scan record.
     *
     * `n` must be in `[UNIT_SIZE, max_supported_size()]` and a
     * multiple of `UNIT_SIZE`. Not `constexpr`: uses `bits::clz`
     * intrinsic via `bits::to_exp_mant` to stay single-cycle on the
     * fast path.
     */
    SNMALLOC_FAST_PATH static const bitmap_info_t&
    bitmap_info_for_request(size_t n)
    {
      SNMALLOC_ASSERT(n >= UNIT_SIZE);
      SNMALLOC_ASSERT((n & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT(n <= max_supported_size());
      size_t raw = bits::to_exp_mant<B, MIN_SIZE_BITS>(n);
      SNMALLOC_ASSERT(raw < MAX_SC);
      return table_.bitmap_info[raw];
    }

    /// Map a request size to its carve record. Preconditions and
    /// properties as `bitmap_info_for_request`.
    SNMALLOC_FAST_PATH static const carve_info_t&
    carve_info_for_request(size_t n)
    {
      SNMALLOC_ASSERT(n >= UNIT_SIZE);
      SNMALLOC_ASSERT((n & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT(n <= max_supported_size());
      size_t raw = bits::to_exp_mant<B, MIN_SIZE_BITS>(n);
      SNMALLOC_ASSERT(raw < MAX_SC);
      return table_.carve_info[raw];
    }

  public:
    /**
     * Bin id of `block`. Operates on arbitrary byte sizes that are
     * multiples of `UNIT_SIZE`, not just exact size classes.
     * `block.size` must be at least `UNIT_SIZE`.
     *
     * A bin id at exponent `e` identifies the *servable set*: the
     * subset of SCs at `e` that `block` could serve. Two blocks with
     * the same servable set at the same exponent share a bin id.
     *
     * The natural byte exponent is `prev_pow2_bits(block.size)`,
     * which ranges over `[MIN_SIZE_BITS, bits::BITS)` once the
     * size is a multiple of `UNIT_SIZE`. The internal exponent
     * `e` is normalised by subtracting `MIN_SIZE_BITS`, so bin
     * 0 always corresponds to the `UNIT_SIZE` block.
     *
     * If alignment padding eats every SC at the natural exponent we
     * drop to `e - 1`, which is guaranteed to fit: its smallest SC
     * has size and alignment `UNIT_SIZE << (e - 1)`, so worst-case
     * `size + pad < UNIT_SIZE << e <= block.size`. One drop is
     * always enough.
     *
     * Not `constexpr`: uses `bits::clz` via `bits::prev_pow2_bits`.
     */
    SNMALLOC_FAST_PATH static size_t bin_index(range_t block)
    {
      SNMALLOC_ASSERT(block.size >= UNIT_SIZE);
      SNMALLOC_ASSERT((block.size & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT((block.base & (UNIT_SIZE - 1)) == 0);

      size_t e = bits::prev_pow2_bits(block.size) - MIN_SIZE_BITS;
      size_t offset = bin_offset_at(block.base, block.size, e);
      if (SNMALLOC_UNLIKELY(offset == BINS_PER_EXP))
      {
        // Padding ate the natural exponent. Drop one and retry. Proof
        // of single-step termination is in the doc comment above.
        SNMALLOC_ASSERT(e > 0);
        e--;
        offset = bin_offset_at(block.base, block.size, e);
        SNMALLOC_ASSERT(offset != BINS_PER_EXP);
      }
      return table_.exp_bin_base[e] + offset;
    }

    /// Largest byte size legal for `carve` / `Bitmap::find_for_request`.
    static constexpr size_t max_supported_size()
    {
      return bits::from_exp_mant<B, MIN_SIZE_BITS>(MAX_SC - 1);
    }

    /**
     * Carve a free block into pre-pad / aligned request / post-pad,
     * delivering exactly `n` bytes to the caller.
     *
     * The carve_info for `n` is used only to find a valid alignment
     * and to verify that the block has room: `req.base` is aligned
     * to `info.align` (the natural alignment of the SC that covers
     * `n`), and the block must contain `info.size` bytes from that
     * point. Only `n` bytes are handed out, and the leftover
     * `info.size - n` bytes roll into `post`. This keeps SC rounding
     * as an arena-internal detail: callers always receive exactly
     * what they asked for.
     *
     * Preconditions (caller must have used `Bitmap::find_for_request`
     * to locate a servable bin):
     *  - `block.size > 0`, `n` in `[UNIT_SIZE, max_supported_size()]`
     *    and a multiple of `UNIT_SIZE`, `block` large enough to fit
     *    the SC after aligning up.
     *  - `block.base + block.size` does not wrap.
     *
     * Pure: does not touch the bitmap or any tree. Either or both
     * `pre` / `post` may have `size == 0`; their `base` is still set
     * to the natural address so `pre.base + pre.size == req.base` and
     * `req.base + req.size == post.base` (keeps caller adjacency
     * checks simple).
     */
    SNMALLOC_FAST_PATH static carve_t carve(range_t block, size_t n)
    {
      SNMALLOC_ASSERT(n >= UNIT_SIZE);
      SNMALLOC_ASSERT((n & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT(n <= max_supported_size());
      SNMALLOC_ASSERT(block.size > 0);
      SNMALLOC_ASSERT((block.size & (UNIT_SIZE - 1)) == 0);
      SNMALLOC_ASSERT((block.base & (UNIT_SIZE - 1)) == 0);
      // Combined with the servability precondition, non-wrapping end
      // ensures the alignment-up below does not wrap either.
      SNMALLOC_ASSERT(block.base + block.size >= block.base);

      const carve_info_t& info = carve_info_for_request(n);

      size_t req_base =
        (block.base + (info.align - 1)) & ~(info.align - 1);
      size_t pre_size = req_base - block.base;

      // Servability precondition: `info.size >= n` bytes fit after
      // `pre`. We only hand out `n`; the remainder (`info.size - n`)
      // joins `post`.
      SNMALLOC_ASSERT(pre_size <= block.size);
      SNMALLOC_ASSERT(block.size - pre_size >= info.size);

      size_t post_base = req_base + n;
      size_t post_size = (block.base + block.size) - post_base;

      carve_t result;
      result.pre = {block.base, pre_size};
      result.req = {req_base, n};
      result.post = {post_base, post_size};
      return result;
    }

    /**
     * Bitmap of non-empty per-arena bins. One bit per bin id
     * (`bin_index`'s output); set iff the corresponding RB-tree is
     * non-empty.
     *
     * Three-method API:
     *   - `add(range_t)`: classify a block and set its bin's bit
     *     (idempotent on the bit; returns the bin id).
     *   - `find_for_request(n)`: smallest set bin whose blocks
     *     all serve `n`, or `SIZE_MAX` if none.
     *   - `clear(bin_id)`: mark empty. Caller must ensure the bin's
     *     tree is actually empty; the bitmap does not track contents.
     *
     * Not thread-safe: callers sharing an arena must serialise the
     * add / find / clear sequence under an external mutex.
     */
    class Bitmap
    {
      friend struct ArenaBinsTestAccess<INTERMEDIATE_BITS, MIN_SIZE_BITS>;

    public:
      /// Strict upper bound on bin ids `bin_index` produces. Exposed
      /// so callers can size parallel arrays (one RB-tree per bin id).
      static constexpr size_t TOTAL_BINS = BINS_PER_EXP * bits::BITS;

      constexpr Bitmap() : words_{} {}

      /**
       * Classify `block`, set its bin's bit, return the bin id.
       *
       * Idempotent on bitmap state: if the bit is already set, this
       * is a no-op (the bin id is still returned).
       *
       * The bitmap does NOT track which `(base, size)` ranges live in
       * each bin's tree — the caller is responsible for inserting
       * `block` into the appropriate tree.
       */
      SNMALLOC_FAST_PATH size_t add(range_t block)
      {
        SNMALLOC_ASSERT(block.size >= UNIT_SIZE);
        SNMALLOC_ASSERT(block.size <= max_supported_size());
        size_t bin_id = bin_index(block);
        SNMALLOC_ASSERT(bin_id < TOTAL_BINS);
        words_[bin_id / bits::BITS] |=
          (size_t(1) << (bin_id & (bits::BITS - 1)));
        return bin_id;
      }

      /// Read-only test: is the bit for `bin_id` set?
      /// Used by `Arena::invariant()`.
      bool test(size_t bin_id) const
      {
        SNMALLOC_ASSERT(bin_id < TOTAL_BINS);
        return (words_[bin_id / bits::BITS] &
                (size_t(1) << (bin_id & (bits::BITS - 1)))) != 0;
      }

      /// Mark bin `bin_id` empty. Caller must ensure the bin's tree
      /// is actually empty; the bitmap does not consult the trees.
      SNMALLOC_FAST_PATH void clear(size_t bin_id)
      {
        SNMALLOC_ASSERT(bin_id < TOTAL_BINS);
        words_[bin_id / bits::BITS] &=
          ~(size_t(1) << (bin_id & (bits::BITS - 1)));
      }

      /**
       * Smallest bin id whose set blocks all serve `n`, or `SIZE_MAX`
       * if none. `n` in `[UNIT_SIZE, max_supported_size()]` and a
       * multiple of `UNIT_SIZE`.
       *
       * Invariant (static_assert below): `BINS_PER_EXP <= bits::BITS`,
       * so the within-exponent range fits inside one word and the
       * search straddles at most one word boundary. After the second
       * word, every remaining word is purely higher-exponent.
       */
      SNMALLOC_FAST_PATH size_t find_for_request(size_t n) const
      {
        const bitmap_info_t& info = bitmap_info_for_request(n);
        SNMALLOC_ASSERT(info.start_word < NUM_BITMAP_WORDS);
        SNMALLOC_ASSUME(info.start_word < NUM_BITMAP_WORDS);

        // First word: start bin + any within-exp neighbours in same word.
        size_t word = info.start_word;
        size_t bits = words_[word] & info.first_mask;
        if (bits != 0)
          return word * bits::BITS + bits::ctz(bits);
        ++word;
        if (word == NUM_BITMAP_WORDS)
          return SIZE_MAX;

        // Second word: within-exp carry plus any higher-exp bits.
        bits = words_[word] & info.second_mask;
        if (bits != 0)
          return word * bits::BITS + bits::ctz(bits);

        // Remaining words: purely higher-exponent, any bit serves.
        while (++word < NUM_BITMAP_WORDS)
          if (words_[word] != 0)
            return word * bits::BITS + bits::ctz(words_[word]);
        return SIZE_MAX;
      }

    private:
      /// Number of size_t words backing the bitmap. Internal layout.
      static constexpr size_t NUM_BITMAP_WORDS =
        (TOTAL_BINS + bits::BITS - 1) / bits::BITS;

      static_assert(
        TOTAL_BINS == BINS_PER_EXP * bits::BITS,
        "Bitmap layout: TOTAL_BINS must be BINS_PER_EXP * bits::BITS so it "
        "divides evenly into bits::BITS-sized words.");
      static_assert(
        NUM_BITMAP_WORDS == BINS_PER_EXP,
        "Bitmap layout: with the canonical TOTAL_BINS, the word count is "
        "exactly BINS_PER_EXP.");
      static_assert(
        TOTAL_BINS < SIZE_MAX,
        "find_for_request returns SIZE_MAX as the 'no match' sentinel; "
        "TOTAL_BINS must be strictly less than SIZE_MAX so no valid bin "
        "id can collide with the sentinel.");
      static_assert(
        BINS_PER_EXP <= bits::BITS,
        "find_for_request assumes the within-exponent range (at most "
        "BINS_PER_EXP bins) fits inside a single word, so the search "
        "straddles at most one word boundary. If a future B pushes "
        "BINS_PER_EXP above bits::BITS, the two-word body must be "
        "generalised to handle a multi-word straddle.");

      size_t words_[NUM_BITMAP_WORDS];
    };

  private:
    // Vocabulary used in the rest of the private implementation:
    //
    //   exponent (e) : the bin-scheme exponent of a size; one axis of
    //                  the size class grid.
    //   mantissa (m) : the within-exponent position, in
    //                  [0, MANTISSAS_PER_EXP). The other axis. When
    //                  passed as a single argument it is named `m`
    //                  (e.g. `start_bin_offset_for_m(m)`).
    //   subset       : a bitmask of mantissas. `bin_subsets[b]` is the
    //                  set of mantissas bin offset `b` can serve.
    //   m_top        : when discussing a particular bin, the maximum
    //                  element of its subset. Used as the bucketing
    //                  axis for the cascade (see `bin_offset_at`).
    //   m_test       : a single-mantissa probe in a cascade step;
    //                  chosen so the probe's outcome disambiguates
    //                  one candidate bin from the rest.

    /**
     * Single source of truth for the bin scheme.
     *
     * `bin_subsets[b]` is a bitmask of the mantissas bin offset `b`
     * can serve: bit `m` set iff bin offset `b`'s servable subset
     * contains mantissa `m`. The canonical bin numbering matches
     * `prototype/skip_analysis.py`. Everything else in this file --
     * `start_bin_offset_for_m`, `serve_mask_for_m`, the per-SC
     * `start_word` / `first_mask` / `second_mask`, and the per-m_top
     * decision lists in `BinTable::cascade_steps` -- is derived
     * (constexpr) from this table.
     *
     * Required invariant (checked at constexpr build time in
     * `BinTable::BinTable`; violating it fails the build): for every
     * `m_top`, the bins whose subset has `m_top` as max element form a
     * strict containment chain when sorted by subset size descending.
     * That is, the largest such subset properly contains the next,
     * which properly contains the one after, and so on. The chain
     * property is what makes the single-mantissa-probe cascade in
     * `bin_offset_at` sufficient to disambiguate among them.
     *
     * If you edit the literals below, re-run
     * `prototype/skip_analysis.py` to verify they still match the
     * canonical numbering and chain property.
     */
    static constexpr ModArray<BINS_PER_EXP, size_t> bin_subsets = []() {
      ModArray<BINS_PER_EXP, size_t> r{};
      if constexpr (B == 1)
      {
        // bin 0: {0}
        // bin 1: {0,1}
        r[0] = 0b01;
        r[1] = 0b11;
      }
      else if constexpr (B == 2)
      {
        // bin 0: {0}      bin 3: {0,1,2}
        // bin 1: {1}      bin 4: {0,1,2,3}
        // bin 2: {0,1}
        r[0] = 0b0001;
        r[1] = 0b0010;
        r[2] = 0b0011;
        r[3] = 0b0111;
        r[4] = 0b1111;
      }
      else /* B == 3 */
      {
        // bin  0: {0}              bin  7: {1,2,3,5}
        // bin  1: {1}              bin  8: {0,1,2,3,4}
        // bin  2: {0,1}            bin  9: {0,1,2,3,5}
        // bin  3: {1,2}            bin 10: {0,1,2,3,4,5}
        // bin  4: {0,1,2}          bin 11: {0,1,2,3,4,5,6}
        // bin  5: {1,2,3}          bin 12: {0,1,2,3,4,5,6,7}
        // bin  6: {0,1,2,3}
        r[0] = 0b00000001;
        r[1] = 0b00000010;
        r[2] = 0b00000011;
        r[3] = 0b00000110;
        r[4] = 0b00000111;
        r[5] = 0b00001110;
        r[6] = 0b00001111;
        r[7] = 0b00101110;
        r[8] = 0b00011111;
        r[9] = 0b00101111;
        r[10] = 0b00111111;
        r[11] = 0b01111111;
        r[12] = 0b11111111;
      }
      return r;
    }();

    /**
     * First within-exponent bin offset whose subset contains mantissa
     * `m`. Derived from `bin_subsets`.
     *
     * Combined with the per-exponent base, this is an SC's absolute
     * start bin index: `start_bit = exp_bin_base[e] +
     * start_bin_offset_for_m(m)`. The bitmap stores its low and high
     * halves pre-shifted into the `bitmap_info_t::first_mask` /
     * `second_mask` fields.
     */
    static constexpr size_t start_bin_offset_for_m(size_t m)
    {
      size_t mask = size_t(1) << m;
      for (size_t b = 0; b < BINS_PER_EXP; b++)
        if (bin_subsets[b] & mask)
          return b;
      return BINS_PER_EXP; // unreachable: every m is in some subset
    }

    /**
     * Bitmask, relative to `start_bin_offset_for_m(m)`, of bins that
     * serve `m`. Bit `k` is set iff bin offset
     * `start_bin_offset_for_m(m) + k` serves a request whose
     * within-exponent position is `m`. The start bin always serves
     * (bit 0 set), within-exponent bins serve iff their subset
     * contains `m`, and bins above the within-exponent range belong
     * to higher exponents and always serve (high bits all 1).
     *
     * Built positively (set bit = "serve") rather than as a "skip"
     * mask: the hot path in `Bitmap::find_for_request` AND's this
     * mask (pre-shifted into `bitmap_info_t::first_mask` / `second_mask`)
     * against the bitmap word without an intermediate NOT.
     */
    static constexpr size_t serve_mask_for_m(size_t m)
    {
      size_t mask = size_t(1) << m;
      size_t start = start_bin_offset_for_m(m);
      size_t result = ~size_t(0);
      for (size_t b = start + 1; b < BINS_PER_EXP; b++)
        if (!(bin_subsets[b] & mask))
          result &= ~(size_t(1) << (b - start));
      return result;
    }

    /// Constexpr popcount: small loop, used only at BinTable build time.
    static constexpr size_t popcount_const(size_t x)
    {
      size_t n = 0;
      while (x != 0)
      {
        n += (x & 1);
        x >>= 1;
      }
      return n;
    }

    /// One step of a per-m_top decision list used by `bin_offset_at`.
    /// If `m_test == NO_TEST` (see below) or `fits(m_test)` is true,
    /// return `bin`.
    struct CascadeStep
    {
      size_t m_test;
      size_t bin;
    };

    /// Sentinel for `CascadeStep::m_test` meaning "take this bin
    /// unconditionally". Any value `>= MANTISSAS_PER_EXP` would do; the
    /// fits() lambda would short-circuit it on `first + m >= past`, but
    /// the explicit sentinel makes the walker's intent obvious and
    /// avoids one unnecessary comparison.
    static constexpr size_t NO_TEST = MANTISSAS_PER_EXP;

    /**
     * Maximum decision-list length per `m_top`. Derived from
     * `bin_subsets`: the largest number of bins sharing the same max
     * subset element. Used to size `cascade_steps[m_top][]`; some
     * `m_top` values have fewer candidates, leaving default-initialised
     * slots at the end. Those slots are never reached because the
     * preceding NO_TEST entry always returns.
     */
    static constexpr size_t MAX_CASCADE_STEPS = []() {
      size_t mx = 0;
      for (size_t m_top = 0; m_top < MANTISSAS_PER_EXP; m_top++)
      {
        size_t cnt = 0;
        for (size_t b = 0; b < BINS_PER_EXP; b++)
        {
          // Bit m_top set and no higher bit set <=> max element is m_top.
          if ((bin_subsets[b] >> m_top) == 1)
            cnt++;
        }
        if (cnt > mx)
          mx = cnt;
      }
      return mx;
    }();

    /**
     * Within-exponent bin offset for a block at byte address `addr`
     * of byte length `n` at internal exponent `e`. Returns
     * `BINS_PER_EXP` (sentinel) if no mantissa at this exponent
     * fits.
     *
     * Walks `m_top` from `MANTISSAS_PER_EXP - 1` down. The first
     * fitting `m_top` is the largest mantissa this block can serve;
     * it is also the natural bucketing axis, because the bins whose
     * subset has `m_top` as max element are exactly the candidates we
     * still need to disambiguate among. `table_.cascade_steps[m_top]`
     * (a constexpr-built decision list, derived from `bin_subsets`)
     * disambiguates among them with at most a couple of secondary
     * `fits` checks.
     *
     * Worst case: `MANTISSAS_PER_EXP + MAX_CASCADE_STEPS - 1` fit
     * checks — the inner loop's last entry is the NO_TEST default and
     * returns without calling `fits`. Typical: 1-2 at the natural
     * exponent and 1 at the fallback exponent.
     */
    SNMALLOC_FAST_PATH static size_t
    bin_offset_at(size_t addr, size_t n, size_t e)
    {
      size_t first = table_.exp_first_sc[e];
      size_t past = table_.exp_first_sc[e + 1];

      auto fits = [&](size_t m) SNMALLOC_FAST_PATH_LAMBDA -> bool {
        // Safety: mantissa m may not exist at this exponent (low
        // regime -- exponents 0..B-1 have fewer than 2^B mantissas;
        // for any B the very first exponent has only 1). Without this
        // check we would index past `past` into the carve_info table.
        if (first + m >= past)
          return false;
        const carve_info_t& ci = table_.carve_info[first + m];
        // Optimisation: near the bottom of n's exponent range the
        // higher-mantissa sizes already exceed n and cannot fit
        // regardless of alignment. Skips the align_up below.
        if (n < ci.size)
          return false;
        size_t pad = bits::align_up(addr, ci.align) - addr;
        return n - ci.size >= pad;
      };

      for (size_t m_top = MANTISSAS_PER_EXP; m_top-- > 0;)
      {
        if (fits(m_top))
        {
          // Walk this m_top's decision list. The list always ends with
          // a NO_TEST entry that acts as the default, so the loop is
          // guaranteed to return.
          for (size_t j = 0; j < MAX_CASCADE_STEPS; j++)
          {
            const CascadeStep& step = table_.cascade_steps[m_top][j];
            if (step.m_test == NO_TEST || fits(step.m_test))
              return step.bin;
          }
          SNMALLOC_ASSERT(false); // unreachable per the invariant above
        }
      }
      return BINS_PER_EXP;
    }

    /**
     * Constexpr-populated rodata tables.
     *
     * `bitmap_info[sc]` is the bitmap-scan record for each in-range
     * sc (consumed by `Bitmap::find_for_request`).
     * `carve_info[sc]` is the size/alignment record for each in-range
     * sc (consumed by `carve` and by `bin_offset_at`'s `fits`
     * predicate during free-side classification).
     * `exp_first_sc[e]` is the first raw sc id at ArenaBins
     * exponent e (with `exp_first_sc[bits::BITS] = MAX_SC` as a sentinel
     * so `[exp_first_sc[e], exp_first_sc[e + 1])` is a valid raw range
     * for every `e < bits::BITS`).
     * `exp_bin_base[e]` is `e * BINS_PER_EXP`, precomputed so the
     * `bin_index` fast path never performs a runtime multiply.
     * `cascade_steps[m_top]` is the decision list `bin_offset_at` walks
     * once it knows `m_top` is the largest fitting mantissa at the
     * current exponent. The list always ends with a NO_TEST entry that
     * acts as the default.
     */
    struct BinTable
    {
      ModArray<MAX_SC, bitmap_info_t> bitmap_info{};
      ModArray<MAX_SC, carve_info_t> carve_info{};
      ModArray<bits::BITS + 1, size_t> exp_first_sc{};
      ModArray<bits::BITS + 1, size_t> exp_bin_base{};
      ModArray<MANTISSAS_PER_EXP, ModArray<MAX_CASCADE_STEPS, CascadeStep>>
        cascade_steps{};

      constexpr BinTable()
      {
        // Boundary tables: keep all (e -> raw sc range) and (e -> bin id
        // base) knowledge in two small ROM arrays. `to_exp_mant_const` is
        // the only place that knows the size class encoding; once we've
        // pinned down the raw boundaries, everything else is table lookup.
        //
        // `e` here is the internal (normalised) exponent: an SC's
        // `e == 0` corresponds to byte size `UNIT_SIZE = 1 << MIN_SIZE_BITS`.
        //
        // Note: `exp_first_sc` does NOT have a uniform stride. At the
        // bottom of the encoding the low regime (no leading-1 bit; the
        // `b = (e == 0) ? 0 : 1` branch in `to_exp_mant_const`) squashes
        // multiple internal exponents into encoded-exponent 0.
        // For `B = 2` the counts are 1, 2, 4, 4, 4, ...
        constexpr size_t MAX_E = bits::BITS - MIN_SIZE_BITS;
        for (size_t e = 0; e < MAX_E; e++)
        {
          exp_first_sc[e] =
            bits::to_exp_mant_const<B, MIN_SIZE_BITS>(size_t(1) << (e + MIN_SIZE_BITS));
          exp_bin_base[e] = e * BINS_PER_EXP;
        }
        exp_first_sc[MAX_E] = MAX_SC;
        exp_bin_base[MAX_E] = MAX_E * BINS_PER_EXP;

        // Per-sc records. Size and alignment come straight from the
        // size-class scheme (via from_exp_mant); start_word, first_mask,
        // second_mask are derived from bin_subsets via the constexpr
        // helpers above, pre-shifted into the bitmap's word layout so
        // the search hot path is two ANDs.
        for (size_t sc = 0; sc < MAX_SC; sc++)
        {
          size_t size = bits::from_exp_mant<B, MIN_SIZE_BITS>(sc);
          size_t e = bits::prev_pow2_bits_const(size) - MIN_SIZE_BITS;
          size_t m = sc - exp_first_sc[e];
          size_t start_bit = exp_bin_base[e] + start_bin_offset_for_m(m);
          size_t mask = serve_mask_for_m(m);
          size_t shift = start_bit & (bits::BITS - 1);
          carve_info[sc].size = size;
          carve_info[sc].align = size & (~size + 1);
          bitmap_info[sc].start_word = start_bit / bits::BITS;
          bitmap_info[sc].first_mask = mask << shift;
          // shift == 0: no within-exponent carry; the second word is
          // entirely higher-exponent. shift > 0: the low `shift` bits
          // receive the top of mask (within-exp carry plus its all-1s
          // tail), and bits [shift, BITS) are higher-exp and always
          // serve.
          bitmap_info[sc].second_mask = (shift == 0) ?
            ~size_t(0) :
            ((mask >> (bits::BITS - shift)) | (~size_t(0) << shift));
        }

        // cascade_steps: for each m_top, build a decision list of
        // (m_test, bin) pairs derived from bin_subsets. Candidates are
        // bins whose subset has m_top as max element; sort descending
        // by subset size. The strict-chain invariant on `bin_subsets`
        // (see its doc comment) guarantees each non-default
        // candidate's subset properly contains the next candidate's,
        // so the discriminator for candidate `i` is one of the
        // mantissas in `bin_subsets[b_i] & ~bin_subsets[b_{i+1}]`.
        for (size_t m_top = 0; m_top < MANTISSAS_PER_EXP; m_top++)
        {
          ModArray<BINS_PER_EXP, size_t> candidates{};
          size_t n_cand = 0;
          for (size_t b = 0; b < BINS_PER_EXP; b++)
          {
            // bin_subsets[b] >> m_top == 1 <=> bit m_top set and no
            // higher bit set <=> max element of subset is m_top.
            if ((bin_subsets[b] >> m_top) == 1)
            {
              candidates[n_cand] = b;
              n_cand++;
            }
          }
          // Insertion sort, descending by popcount of subset.
          for (size_t i = 1; i < n_cand; i++)
          {
            size_t b = candidates[i];
            size_t pcb = popcount_const(bin_subsets[b]);
            size_t j = i;
            while (j > 0 &&
                   popcount_const(bin_subsets[candidates[j - 1]]) < pcb)
            {
              candidates[j] = candidates[j - 1];
              j--;
            }
            candidates[j] = b;
          }
          // Non-default candidates: pick a discriminating mantissa.
          // Under the strict-chain invariant on `bin_subsets`, each
          // candidate's subset properly contains the next candidate's,
          // so `bin_subsets[b] & ~bin_subsets[b_next]` is the
          // (non-empty) set of mantissas unique to this candidate.
          for (size_t i = 0; i + 1 < n_cand; i++)
          {
            size_t b = candidates[i];
            size_t b_next = candidates[i + 1];
            size_t discrim_set = bin_subsets[b] & ~bin_subsets[b_next];
            // If this fires, `bin_subsets` violates the strict-chain
            // invariant: candidate `b`'s subset does not properly
            // contain candidate `b_next`'s, so the cascade can't be
            // expressed as single-mantissa probes. Calling the
            // non-constexpr `SNMALLOC_CHECK` makes the constexpr
            // evaluation non-constant and surfaces the violation as
            // a compile error.
            if (discrim_set == 0)
              SNMALLOC_CHECK_MSG(
                false, "bin_subsets violates strict-chain invariant");
            cascade_steps[m_top][i].m_test = bits::ctz_const(discrim_set);
            cascade_steps[m_top][i].bin = b;
          }
          // Default (last) candidate.
          cascade_steps[m_top][n_cand - 1].m_test = NO_TEST;
          cascade_steps[m_top][n_cand - 1].bin = candidates[n_cand - 1];
        }
      }
    };

    static constexpr BinTable table_{};
  };
} // namespace snmalloc
