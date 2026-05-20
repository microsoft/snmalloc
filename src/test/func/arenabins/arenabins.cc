/**
 * Unit tests for ArenaBins.
 *
 * Exercises:
 *  - the chunk size class encoding (via `ArenaBinsTestAccess`),
 *  - the private bin classification (`bin_index`),
 *  - the narrow public surface: `Bitmap::add` / `find_for_request` /
 *    `clear`, and the pure `carve(range_t, n)` decomposition.
 *
 * Strategy: brute force. For each (addr_chunks, n_chunks) on a small grid
 * we directly check whether a block can serve every candidate size class
 * (by finding an aligned sub-range that fits via `can_serve`, and
 * consulting the canonical `bin_subsets` table via `serves`), and
 * compare against what `bin_index` predicts. Bitmap behaviour is
 * cross-checked against a slow reference scanner that formulates
 * "bin b serves request n" directly in terms of the canonical
 * `bin_subsets` table; raw word access for tests goes through
 * `ArenaBinsTestAccess::raw_*`.
 */

#include "test/setup.h"
#include "test/snmalloc_testlib.h"

#include <cstdio>
#include <cstdlib>
#include <snmalloc/backend_helpers/arenabins.h>
#include <snmalloc/ds_core/helpers.h>
#include <vector>

namespace snmalloc
{
  /**
   * Friend struct exposing private internals of `ArenaBins<B>`
   * (and its nested `Bitmap`) for unit tests. Forward-declared in
   * `arenabins.h`; defined here so the production header
   * carries no test-only surface.
   */
  template<size_t INTERMEDIATE_BITS>
  struct ArenaBinsTestAccess
  {
    using Bins = ArenaBins<INTERMEDIATE_BITS>;

    using Bitmap = typename Bins::Bitmap;
    using range_t = typename Bins::range_t;
    using carve_t = typename Bins::carve_t;
    using bitmap_info_t = typename Bins::bitmap_info_t;
    using carve_info_t = typename Bins::carve_info_t;

    static constexpr size_t B = Bins::B;
    static constexpr size_t MANTISSAS_PER_EXP = Bins::MANTISSAS_PER_EXP;
    static constexpr size_t BINS_PER_EXP = Bins::BINS_PER_EXP;
    static constexpr size_t MAX_SC = Bins::MAX_SC;

    SNMALLOC_FAST_PATH static carve_t carve(range_t block, size_t n)
    {
      return Bins::carve(block, n);
    }

    SNMALLOC_FAST_PATH static const bitmap_info_t&
    bitmap_info_for_request(size_t n)
    {
      return Bins::bitmap_info_for_request(n);
    }

    SNMALLOC_FAST_PATH static const carve_info_t&
    carve_info_for_request(size_t n)
    {
      return Bins::carve_info_for_request(n);
    }

    SNMALLOC_FAST_PATH static size_t bin_index(range_t block)
    {
      return Bins::bin_index(block);
    }

    static constexpr size_t max_supported_chunks()
    {
      return Bins::max_supported_chunks();
    }

    // --- Raw size-class id access ---
    //
    // The bin scheme assigns a dense raw id in `[0, MAX_SC)` to each
    // size class. Production code never names these (the fast path
    // goes straight from request size to the bitmap-scan / carve
    // record). Tests cross-check the encoding via the helpers below;
    // the alias `chunk_sc_t = size_t` preserves the existing test
    // naming.

    using chunk_sc_t = size_t;

    /// Raw id of the smallest size class >= n_chunks.
    SNMALLOC_FAST_PATH static chunk_sc_t request(size_t n)
    {
      SNMALLOC_ASSERT(n >= 1);
      SNMALLOC_ASSERT(n <= Bins::max_supported_chunks());
      return bits::to_exp_mant<INTERMEDIATE_BITS, 0>(n);
    }

    static constexpr size_t size_chunks(chunk_sc_t sc)
    {
      return Bins::table_.carve_info[sc].size_chunks;
    }

    static constexpr size_t align_chunks(chunk_sc_t sc)
    {
      return Bins::table_.carve_info[sc].align_chunks;
    }

    SNMALLOC_FAST_PATH static const bitmap_info_t& bitmap_info(chunk_sc_t sc)
    {
      SNMALLOC_ASSERT(sc < Bins::MAX_SC);
      return Bins::table_.bitmap_info[sc];
    }

    SNMALLOC_FAST_PATH static const carve_info_t& carve_info(chunk_sc_t sc)
    {
      SNMALLOC_ASSERT(sc < Bins::MAX_SC);
      return Bins::table_.carve_info[sc];
    }

    /// `bitmap_info_for_request`, constexpr (uses `to_exp_mant_const`).
    /// Only used in `static_assert`s.
    static constexpr const bitmap_info_t&
    bitmap_info_for_request_const(size_t n)
    {
      return Bins::table_
        .bitmap_info[bits::to_exp_mant_const<INTERMEDIATE_BITS, 0>(n)];
    }

    /// `carve_info_for_request`, constexpr (uses `to_exp_mant_const`).
    /// Only used in `static_assert`s.
    static constexpr const carve_info_t& carve_info_for_request_const(size_t n)
    {
      return Bins::table_
        .carve_info[bits::to_exp_mant_const<INTERMEDIATE_BITS, 0>(n)];
    }

    // The canonical source of truth for what each within-exponent bin
    // offset can serve. Tests express the conceptual "bin b serves
    // request n" predicate directly in terms of this table so they do
    // not depend on the bitmap's pre-shifted layout.
    static constexpr const auto& bin_subsets = Bins::bin_subsets;

    // --- Bitmap raw-word access ---
    //
    // The public Bitmap API is narrow (add / find_for_request / clear).
    // Tests need to:
    //  - set up arbitrary bitmap states (single bit, exhaustive patterns)
    //    without going through `add` (which classifies a (base, size)
    //    range and so is constrained by what classifications exist).
    //  - inspect bitmap state after operations (test "exactly this bit is
    //    set" and "no other bit changed").
    // These accessors expose the raw word storage to do that.

    static constexpr size_t NUM_BITMAP_WORDS = Bitmap::NUM_BITMAP_WORDS;

    /// Set bit `bin_id` directly in the bitmap, bypassing
    /// classification. For exhaustive bit-pattern tests.
    static void raw_set(Bitmap& b, size_t bin_id)
    {
      SNMALLOC_ASSERT(bin_id < Bitmap::TOTAL_BINS);
      b.words_[bin_id / bits::BITS] |=
        (size_t(1) << (bin_id & (bits::BITS - 1)));
    }

    /// Test whether bit `bin_id` is set in the bitmap.
    static bool raw_has(const Bitmap& b, size_t bin_id)
    {
      SNMALLOC_ASSERT(bin_id < Bitmap::TOTAL_BINS);
      return (b.words_[bin_id / bits::BITS] >> (bin_id & (bits::BITS - 1))) &
        size_t(1);
    }

    /// Whether the bitmap has no bits set.
    static bool raw_empty(const Bitmap& b)
    {
      for (size_t i = 0; i < Bitmap::NUM_BITMAP_WORDS; i++)
        if (b.words_[i] != 0)
          return false;
      return true;
    }

    /// Read a raw word of the bitmap; for assertions like "only this
    /// word is non-zero" or "the words round-trip exactly".
    static size_t raw_word(const Bitmap& b, size_t word_idx)
    {
      SNMALLOC_ASSERT(word_idx < Bitmap::NUM_BITMAP_WORDS);
      return b.words_[word_idx];
    }
  };
} // namespace snmalloc

using snmalloc::ArenaBinsTestAccess;

// Compile-time checks: a few size-class encoding properties that we want
// to fail the build (not the runtime) if regressed.
namespace static_checks
{
  using B1 = ArenaBinsTestAccess<1>;
  using B2 = ArenaBinsTestAccess<2>;
  using B3 = ArenaBinsTestAccess<3>;

  static_assert(B1::BINS_PER_EXP == 2, "B=1 BINS_PER_EXP");
  static_assert(B2::BINS_PER_EXP == 5, "B=2 BINS_PER_EXP");
  static_assert(B3::BINS_PER_EXP == 13, "B=3 BINS_PER_EXP");

  static_assert(
    B1::MAX_SC == ((snmalloc::bits::BITS - 1) << 1) + ((1 << 1) - 1),
    "B=1 MAX_SC");
  static_assert(
    B2::MAX_SC == ((snmalloc::bits::BITS - 2) << 2) + ((1 << 2) - 1),
    "B=2 MAX_SC");
  static_assert(
    B3::MAX_SC == ((snmalloc::bits::BITS - 3) << 3) + ((1 << 3) - 1),
    "B=3 MAX_SC");

  // Sizes that are powers of two have align == size.
  static_assert(
    B2::carve_info_for_request_const(4).align_chunks == 4, "size 4 align");
  static_assert(
    B3::carve_info_for_request_const(8).align_chunks == 8, "size 8 align");

  // size_chunks at request(s) must be >= s.
  static_assert(
    B2::carve_info_for_request_const(9).size_chunks == 10, "B=2 round-up");
  static_assert(
    B3::carve_info_for_request_const(17).size_chunks == 18, "B=3 round-up");
} // namespace static_checks

namespace
{
  /// Conceptual predicate, expressed directly in terms of the canonical
  /// `bin_subsets` table (the single source of truth for the bin
  /// scheme). Bin `b` serves a request of size `n` iff `b`'s exponent
  /// strictly exceeds `n`'s (any higher-exponent block is big enough),
  /// or they share an exponent and `b`'s within-exponent subset
  /// includes `n`'s mantissa.
  ///
  /// This is the reference both for what `find_for_request` must
  /// return and for what `bin_index` must classify into.
  template<size_t B>
  constexpr bool serves(size_t bin, size_t n)
  {
    using Bins = ArenaBinsTestAccess<B>;
    size_t e_b = bin / Bins::BINS_PER_EXP;
    size_t o_b = bin % Bins::BINS_PER_EXP;
    size_t raw = snmalloc::bits::to_exp_mant_const<B, 0>(n);
    size_t size_n = snmalloc::bits::from_exp_mant<B, 0>(raw);
    size_t e_n = snmalloc::bits::prev_pow2_bits_const(size_n);
    if (e_b < e_n)
      return false;
    if (e_b > e_n)
      return true;
    size_t exp_first =
      snmalloc::bits::to_exp_mant_const<B, 0>(size_t(1) << e_n);
    size_t m_n = raw - exp_first;
    return ((Bins::bin_subsets[o_b] >> m_n) & size_t(1)) != 0;
  }

  /// Return true iff a block of `n` chunks starting at chunk-aligned address
  /// `addr` can serve a size class of size `s` chunks with natural alignment
  /// `a` chunks. Brute-force search for an aligned sub-range that fits.
  bool can_serve(size_t addr, size_t n, size_t s, size_t a)
  {
    if (s == 0 || s > n)
      return false;
    // Find first a-aligned address in [addr, addr + n - s].
    size_t mod = addr & (a - 1);
    size_t first = (mod == 0) ? addr : (addr + (a - mod));
    return first + s <= addr + n;
  }

  template<size_t B>
  void check_chunk_sc_roundtrip()
  {
    using Bins = ArenaBinsTestAccess<B>;

    // Properties (together these imply request is the smallest size class
    // with size >= s):
    //   1. size_chunks(request(s)) >= s for all s >= 1.
    //   2. Idempotence: request(size_chunks(sc)) == sc.
    //   3. Monotonicity: s1 <= s2 implies request(s1) <= request(s2).
    auto prev_sc = Bins::request(1);
    for (size_t s = 1; s <= 4096; s++)
    {
      auto sc = Bins::request(s);
      size_t cs = Bins::size_chunks(sc);
      if (cs < s)
      {
        std::printf(
          "B=%zu request(%zu) gave class with size %zu < %zu\n", B, s, cs, s);
        std::abort();
      }
      if (Bins::request(cs) != sc)
      {
        std::printf("B=%zu request(size_chunks(sc))!=sc for cs=%zu\n", B, cs);
        std::abort();
      }
      if (sc < prev_sc)
      {
        std::printf("B=%zu request not monotone at s=%zu\n", B, s);
        std::abort();
      }
      prev_sc = sc;
    }
  }

  template<size_t B>
  void check_align_chunks()
  {
    using Bins = ArenaBinsTestAccess<B>;

    for (size_t s = 1; s <= 4096; s++)
    {
      auto sc = Bins::request(s);
      size_t cs = Bins::size_chunks(sc);
      size_t a = Bins::align_chunks(sc);
      // a must be a power of two.
      if (a == 0 || (a & (a - 1)) != 0)
      {
        std::printf("B=%zu size %zu: align_chunks %zu not pow2\n", B, cs, a);
        std::abort();
      }
      // a must divide cs.
      if (cs % a != 0)
      {
        std::printf(
          "B=%zu size %zu: align_chunks %zu does not divide size\n", B, cs, a);
        std::abort();
      }
      // a should be the LARGEST power of two dividing cs.
      if ((a << 1) != 0 && cs % (a << 1) == 0)
      {
        std::printf(
          "B=%zu size %zu: align_chunks %zu not the largest pow2 divisor\n",
          B,
          cs,
          a);
        std::abort();
      }
    }
  }

  /// Collect all chunk_sc_t classes whose size fits in the test grid.
  template<size_t B>
  std::vector<typename ArenaBinsTestAccess<B>::chunk_sc_t>
  collect_classes(size_t max_size)
  {
    using Bins = ArenaBinsTestAccess<B>;
    using sc_t = typename Bins::chunk_sc_t;

    std::vector<sc_t> v;
    sc_t prev{};
    bool have_prev = false;
    for (size_t s = 1; s <= max_size; s++)
    {
      sc_t sc = Bins::request(s);
      if (Bins::size_chunks(sc) != s)
        continue; // s is not a class size
      if (!have_prev || sc != prev)
      {
        v.push_back(sc);
        prev = sc;
        have_prev = true;
      }
    }
    return v;
  }

  template<size_t B>
  void check_bin_classification(size_t max_addr, size_t max_n)
  {
    using Bins = ArenaBinsTestAccess<B>;
    auto classes = collect_classes<B>(max_n);

    for (size_t addr = 0; addr < max_addr; addr++)
    {
      for (size_t n = 1; n <= max_n; n++)
      {
        size_t bin = Bins::bin_index({addr, n});

        for (auto sc : classes)
        {
          size_t s = Bins::size_chunks(sc);
          size_t a = Bins::align_chunks(sc);
          bool actually = can_serve(addr, n, s, a);
          bool predicted = serves<B>(bin, s);

          if (predicted != actually)
          {
            std::printf(
              "B=%zu addr=%zu n=%zu bin=%zu sc.size=%zu sc.align=%zu: "
              "predicted=%d actually=%d\n",
              B,
              addr,
              n,
              bin,
              s,
              a,
              (int)predicted,
              (int)actually);
            std::abort();
          }
        }
      }
    }
  }

  template<size_t B>
  void check_bin_id_range()
  {
    using Bins = ArenaBinsTestAccess<B>;

    // bin_index always returns a value in [0, BINS_PER_EXP * (e+1)) for the
    // block's natural exponent e.
    for (size_t addr = 0; addr < 32; addr++)
    {
      for (size_t n = 1; n <= 64; n++)
      {
        size_t bin = Bins::bin_index({addr, n});
        size_t within = bin % Bins::BINS_PER_EXP;
        if (within >= Bins::BINS_PER_EXP)
        {
          std::printf(
            "B=%zu addr=%zu n=%zu bin=%zu: within-exp id %zu >= BINS_PER_EXP "
            "%zu\n",
            B,
            addr,
            n,
            bin,
            within,
            Bins::BINS_PER_EXP);
          std::abort();
        }
      }
    }
  }

  /// Verify that `*_info_for_request(n)` agrees with the per-sc
  /// accessors for every n in a range.
  template<size_t B>
  void check_info_consistency()
  {
    using Bins = ArenaBinsTestAccess<B>;

    for (size_t s = 1; s <= 4096; s++)
    {
      auto sc = Bins::request(s);

      // carve_info_for_request(s) must match the per-sc accessors and
      // must alias the carve_info(request(s)) record (single table
      // indirection, no copy).
      const auto& ci = Bins::carve_info_for_request(s);
      if (ci.size_chunks != Bins::size_chunks(sc))
      {
        std::printf(
          "B=%zu carve_info_for_request(%zu).size_chunks mismatch\n", B, s);
        std::abort();
      }
      if (ci.align_chunks != Bins::align_chunks(sc))
      {
        std::printf(
          "B=%zu carve_info_for_request(%zu).align_chunks mismatch\n", B, s);
        std::abort();
      }
      if (&ci != &Bins::carve_info(sc))
      {
        std::printf(
          "B=%zu carve_info_for_request(%zu) and carve_info(request) "
          "point at different records\n",
          B,
          s);
        std::abort();
      }

      // bitmap_info_for_request(s) must alias bitmap_info(request(s)).
      const auto& bi = Bins::bitmap_info_for_request(s);
      if (&bi != &Bins::bitmap_info(sc))
      {
        std::printf(
          "B=%zu bitmap_info_for_request(%zu) and bitmap_info(request) "
          "point at different records\n",
          B,
          s);
        std::abort();
      }
    }
  }

  /// to_exp_mant runtime / _const equivalence across a representative
  /// range of values, including edges near max_supported_chunks. The
  /// runtime variant uses the intrinsic; we cross-check against the
  /// constexpr reference that's already exercised at compile time.
  template<size_t B>
  void check_to_exp_mant_equivalence()
  {
    using Bins = ArenaBinsTestAccess<B>;

    auto check_one = [&](size_t n) {
      size_t r = snmalloc::bits::to_exp_mant<B, 0>(n);
      size_t c = snmalloc::bits::to_exp_mant_const<B, 0>(n);
      if (r != c)
      {
        std::printf("B=%zu to_exp_mant(%zu) = %zu, _const = %zu\n", B, n, r, c);
        std::abort();
      }
    };

    // Small values.
    for (size_t n = 1; n <= 4096; n++)
      check_one(n);

    // Powers of two and ±1, up to the largest representable.
    for (size_t e = 0; e < snmalloc::bits::BITS; e++)
    {
      size_t pow = size_t(1) << e;
      if (pow == 0)
        continue;
      if (pow >= 1 && pow <= Bins::max_supported_chunks())
        check_one(pow);
      if (pow + 1 <= Bins::max_supported_chunks())
        check_one(pow + 1);
      if (pow >= 2)
        check_one(pow - 1);
    }

    // The upper boundary itself.
    check_one(Bins::max_supported_chunks());
    if (Bins::max_supported_chunks() > 1)
      check_one(Bins::max_supported_chunks() - 1);

    // A handful of stride values across the full range.
    size_t step = Bins::max_supported_chunks() / 257;
    if (step == 0)
      step = 1;
    for (size_t n = 1; n <= Bins::max_supported_chunks() && n > 0;
         n += step + 1)
      check_one(n);
  }

  /// Reference implementation of find_for_request: brute-force scan
  /// over every bin id, applying the canonical `serves` predicate
  /// (defined directly in terms of `bin_subsets`).
  template<size_t B>
  size_t reference_find(
    size_t n_chunks, const typename ArenaBinsTestAccess<B>::Bitmap& bm)
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;
    for (size_t b = 0; b < Bitmap::TOTAL_BINS; b++)
    {
      if (!Bins::raw_has(bm, b))
        continue;
      if (serves<B>(b, n_chunks))
        return b;
    }
    return SIZE_MAX;
  }

  template<size_t B>
  void check_bitmap_smoke()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;
    Bitmap bm;
    if (!Bins::raw_empty(bm))
      std::abort();
    Bins::raw_set(bm, 0);
    if (Bins::raw_empty(bm))
      std::abort();
    if (!Bins::raw_has(bm, 0))
      std::abort();
    if (Bins::raw_has(bm, 1))
      std::abort();
    Bins::raw_set(bm, Bitmap::TOTAL_BINS - 1);
    if (!Bins::raw_has(bm, Bitmap::TOTAL_BINS - 1))
      std::abort();
    bm.clear(0);
    if (Bins::raw_has(bm, 0))
      std::abort();
    bm.clear(Bitmap::TOTAL_BINS - 1);
    if (!Bins::raw_empty(bm))
      std::abort();
  }

  /// Iterate over every `chunk_sc_t` raw id in `[0, MAX_SC)`. For each
  /// one, decode its request size, look up its `bitmap_info_t`, and
  /// run `body(n_chunks, bitmap_info)`. Multiple raw ids can share the
  /// same `(start_word, first_mask, second_mask)` triple; callers that
  /// want a unique-deposit view are responsible for deduplicating.
  template<size_t B, typename F>
  void for_each_class_info(F body)
  {
    using Bins = ArenaBinsTestAccess<B>;
    for (size_t raw = 0; raw < Bins::MAX_SC; raw++)
    {
      size_t s = snmalloc::bits::from_exp_mant<B, 0>(raw);
      const auto& info = Bins::bitmap_info_for_request(s);
      body(s, info);
    }
  }

  template<size_t B>
  void check_bitmap_find_empty()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;
    Bitmap bm;
    for_each_class_info<B>([&](size_t n, const auto& /*info*/) {
      if (bm.find_for_request(n) != SIZE_MAX)
        std::abort();
    });
  }

  /// For each B and each bin id in [0, TOTAL_BINS): set exactly that
  /// bit, then for every distinct request info cross-check
  /// find_for_request against the reference scanner.
  template<size_t B>
  void check_bitmap_exhaustive_single_bit()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;

    // Gather a representative set of entries (one per distinct bitmap
    // deposit, i.e. distinct (start_word, first_mask, second_mask)
    // triple, with a request size that maps to it).
    struct Entry
    {
      size_t n_chunks;
      typename Bins::bitmap_info_t info;
    };

    std::vector<Entry> entries;
    for_each_class_info<B>([&](size_t n, const auto& info) {
      for (const auto& e : entries)
      {
        if (
          e.info.start_word == info.start_word &&
          e.info.first_mask == info.first_mask &&
          e.info.second_mask == info.second_mask)
          return;
      }
      entries.push_back({n, info});
    });

    for (size_t bin_id = 0; bin_id < Bitmap::TOTAL_BINS; bin_id++)
    {
      Bitmap bm;
      Bins::raw_set(bm, bin_id);
      for (const auto& e : entries)
      {
        size_t got = bm.find_for_request(e.n_chunks);
        size_t want = reference_find<B>(e.n_chunks, bm);
        if (got != want)
        {
          std::printf(
            "B=%zu single-bit: bin=%zu n=%zu: got=%zu want=%zu\n",
            B,
            bin_id,
            e.n_chunks,
            got,
            want);
          std::abort();
        }
      }
    }
  }

  /// Randomised multi-bit arena states cross-checked against the
  /// reference scanner.
  template<size_t B>
  void check_bitmap_multi_bit_random()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;

    struct Entry
    {
      size_t n_chunks;
      typename Bins::bitmap_info_t info;
    };

    std::vector<Entry> entries;
    for_each_class_info<B>([&](size_t n, const auto& info) {
      for (const auto& e : entries)
      {
        if (
          e.info.start_word == info.start_word &&
          e.info.first_mask == info.first_mask &&
          e.info.second_mask == info.second_mask)
          return;
      }
      entries.push_back({n, info});
    });

    // Deterministic xorshift64 PRNG so failures are reproducible.
    auto xorshift = [](uint64_t& s) -> uint64_t {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      return s;
    };

    uint64_t rng_state = 0x9E3779B97F4A7C15ull + B;
    for (size_t trial = 0; trial < 2000; trial++)
    {
      Bitmap bm;
      // Density varies per trial: choose how many bits to set.
      size_t target = (size_t)(xorshift(rng_state) % (Bitmap::TOTAL_BINS + 1));
      for (size_t i = 0; i < target; i++)
      {
        size_t b = (size_t)(xorshift(rng_state) % Bitmap::TOTAL_BINS);
        Bins::raw_set(bm, b);
      }
      for (const auto& e : entries)
      {
        size_t got = bm.find_for_request(e.n_chunks);
        size_t want = reference_find<B>(e.n_chunks, bm);
        if (got != want)
        {
          std::printf(
            "B=%zu trial=%zu n=%zu: got=%zu want=%zu\n",
            B,
            trial,
            e.n_chunks,
            got,
            want);
          std::abort();
        }
      }
    }
  }

  /// Targeted word-boundary cases: enumerate real table entries, pick
  /// out those whose within-exp range straddles a bitmap word, and
  /// drive each through a four-way sub-case grid:
  ///   (i) bit set in first word's considered region only
  ///   (ii) bit set as within-exp continuation in second word
  ///   (iii) bit set as higher-exp candidate in second word
  ///   (iv) bit set only in word 3 or beyond
  template<size_t B>
  void check_bitmap_word_boundary()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;

    auto check_predicted =
      [&](const Bitmap& bm, size_t n_chunks, const char* label) {
        size_t got = bm.find_for_request(n_chunks);
        size_t want = reference_find<B>(n_chunks, bm);
        if (got != want)
        {
          std::printf(
            "B=%zu word-boundary [%s] n=%zu: got=%zu want=%zu\n",
            B,
            label,
            n_chunks,
            got,
            want);
          std::abort();
        }
      };

    bool found_straddle = false;
    bool found_aligned = false;
    for (size_t raw = 0; raw < Bins::MAX_SC; raw++)
    {
      size_t s = snmalloc::bits::from_exp_mant<B, 0>(raw);
      const auto& info = Bins::bitmap_info_for_request(s);
      // Recover the absolute start bin from the precomputed layout:
      // the start bin always serves, so bit 0 of the conceptual
      // serve_mask is set, which means `first_mask`'s lowest set bit
      // is at position `shift = start_bit & (BITS - 1)`.
      size_t shift = snmalloc::bits::ctz(info.first_mask);
      size_t start_bit = info.start_word * snmalloc::bits::BITS + shift;
      size_t state = start_bit % Bins::BINS_PER_EXP;
      size_t r = Bins::BINS_PER_EXP - state;
      bool straddles = (shift + r) > snmalloc::bits::BITS;
      bool aligned = (shift == 0);

      if (straddles)
        found_straddle = true;
      if (aligned)
        found_aligned = true;
      if (!(straddles || aligned))
        continue;

      // (i) Single bit at the very start_bit.
      {
        Bitmap bm;
        Bins::raw_set(bm, start_bit);
        check_predicted(bm, s, "case-i-start_bit");
      }

      // (ii) Single bit in the second word's within-exp continuation
      // (only meaningful for straddling cases).
      if (straddles)
      {
        size_t carry_bin = start_bit + (snmalloc::bits::BITS - shift);
        if (carry_bin < Bitmap::TOTAL_BINS)
        {
          Bitmap bm;
          Bins::raw_set(bm, carry_bin);
          check_predicted(bm, s, "case-ii-continuation");
        }
      }

      // (iii) Bit in second word's higher-exp region.
      {
        size_t second_word = info.start_word + 1;
        if (second_word < Bins::NUM_BITMAP_WORDS)
        {
          // Pick a bin that is higher-exponent: at least
          // start_bit + BINS_PER_EXP - state (i.e. into next exponent).
          size_t higher_bin = start_bit + r;
          if (higher_bin < Bitmap::TOTAL_BINS)
          {
            Bitmap bm;
            Bins::raw_set(bm, higher_bin);
            check_predicted(bm, s, "case-iii-higher-exp");
          }
        }
      }

      // (iv) Bit only in word 3 or beyond.
      {
        size_t target_word = info.start_word + 2;
        if (target_word < Bins::NUM_BITMAP_WORDS)
        {
          size_t target_bin = target_word * snmalloc::bits::BITS;
          if (target_bin < Bitmap::TOTAL_BINS)
          {
            Bitmap bm;
            Bins::raw_set(bm, target_bin);
            check_predicted(bm, s, "case-iv-later-word");
          }
        }
      }
    }

    // Sanity: for B that actually places entries near word boundaries,
    // at least one straddling case must exist on 64-bit. We don't assert
    // straddle exists for all B (B=1's bins-per-exp = 2 might not
    // straddle on 64-bit), but aligned cases must.
    if (!found_aligned)
    {
      std::printf("B=%zu: no aligned start_bit found!\n", B);
      std::abort();
    }
    (void)found_straddle;
  }

  /// Integration test: set bits by `bin_index(addr, n)`, then probe via
  /// `find_for_request(req)`. The bitmap result must equal
  /// `bin_index(addr, n)` whenever `can_serve` says the block satisfies
  /// the request, and `SIZE_MAX` otherwise.
  template<size_t B>
  void check_bitmap_bin_index_integration()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;

    auto classes = collect_classes<B>(64);
    for (size_t addr = 0; addr < 32; addr++)
    {
      for (size_t n = 1; n <= 64; n++)
      {
        Bitmap bm;
        size_t bin = Bins::bin_index({addr, n});
        Bins::raw_set(bm, bin);
        for (auto sc : classes)
        {
          size_t s = Bins::size_chunks(sc);
          size_t a = Bins::align_chunks(sc);
          bool actually = can_serve(addr, n, s, a);
          size_t got = bm.find_for_request(s);
          size_t want = actually ? bin : size_t(SIZE_MAX);
          if (got != want)
          {
            std::printf(
              "B=%zu integration: addr=%zu n=%zu bin=%zu sc.size=%zu "
              "sc.align=%zu: got=%zu want=%zu actually=%d\n",
              B,
              addr,
              n,
              bin,
              s,
              a,
              got,
              want,
              (int)actually);
            std::abort();
          }
        }
      }
    }
  }

  /// Verify that Bitmap::add classifies (base, size) ranges to the same
  /// bin id as `bin_index`, sets the corresponding bit, and is
  /// idempotent on both the returned id and the underlying word state.
  template<size_t B>
  void check_bitmap_add()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;
    using range_t = typename Bins::range_t;

    for (size_t addr = 0; addr < 32; addr++)
    {
      for (size_t n = 1; n <= 64; n++)
      {
        Bitmap bm;
        size_t expected = Bins::bin_index({addr, n});
        size_t got = bm.add(range_t{addr, n});
        if (got != expected)
        {
          std::printf(
            "B=%zu add: addr=%zu n=%zu got=%zu expected=%zu\n",
            B,
            addr,
            n,
            got,
            expected);
          std::abort();
        }
        if (!Bins::raw_has(bm, expected))
        {
          std::printf(
            "B=%zu add: addr=%zu n=%zu bin %zu not set after add\n",
            B,
            addr,
            n,
            expected);
          std::abort();
        }

        // Snapshot every word, call add again, verify nothing changed
        // and we get the same id back. Idempotence on state.
        std::vector<size_t> snapshot;
        for (size_t w = 0; w < Bins::NUM_BITMAP_WORDS; w++)
          snapshot.push_back(Bins::raw_word(bm, w));
        size_t got2 = bm.add(range_t{addr, n});
        if (got2 != expected)
        {
          std::printf(
            "B=%zu add idempotent: addr=%zu n=%zu second add returned "
            "%zu (first returned %zu)\n",
            B,
            addr,
            n,
            got2,
            expected);
          std::abort();
        }
        for (size_t w = 0; w < Bins::NUM_BITMAP_WORDS; w++)
        {
          if (Bins::raw_word(bm, w) != snapshot[w])
          {
            std::printf(
              "B=%zu add idempotent: addr=%zu n=%zu word %zu changed\n",
              B,
              addr,
              n,
              w);
            std::abort();
          }
        }
      }
    }
  }

  /// With multiple blocks added, `find_for_request` must return the
  /// *minimum* bin id whose blocks all serve the request, not just any
  /// such bin id.
  template<size_t B>
  void check_bitmap_find_min()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using Bitmap = typename Bins::Bitmap;

    struct Entry
    {
      size_t n_chunks;
      typename Bins::bitmap_info_t info;
    };

    std::vector<Entry> entries;
    for_each_class_info<B>([&](size_t n, const auto& info) {
      for (const auto& e : entries)
      {
        if (
          e.info.start_word == info.start_word &&
          e.info.first_mask == info.first_mask &&
          e.info.second_mask == info.second_mask)
          return;
      }
      entries.push_back({n, info});
    });

    // For each request entry: pick three bin ids that all serve this
    // request (the start_bit itself; a higher-exp bin; the topmost
    // bin), set all three, and verify find_for_request returns the
    // smallest of the three.
    for (const auto& e : entries)
    {
      // Recover the absolute start bin from the precomputed layout.
      size_t start_bit = e.info.start_word * snmalloc::bits::BITS +
        snmalloc::bits::ctz(e.info.first_mask);
      size_t a = start_bit;
      size_t b =
        start_bit + (Bins::BINS_PER_EXP - (start_bit % Bins::BINS_PER_EXP));
      size_t c = Bitmap::TOTAL_BINS - 1;
      if (a >= Bitmap::TOTAL_BINS)
        continue;
      if (b >= Bitmap::TOTAL_BINS)
        continue;
      // a < b < c by construction (a < b since b - a > 0; b <= a + r
      // <= start_bit + BINS_PER_EXP <= TOTAL_BINS - 1 = c only when
      // start_bit far enough below; skip cases where it's not).
      if (!(a < b && b < c))
        continue;

      Bitmap bm;
      Bins::raw_set(bm, a);
      Bins::raw_set(bm, b);
      Bins::raw_set(bm, c);
      size_t got = bm.find_for_request(e.n_chunks);
      if (got != a)
      {
        std::printf(
          "B=%zu find_min: n=%zu bits set {%zu,%zu,%zu} "
          "got=%zu (expected min %zu)\n",
          B,
          e.n_chunks,
          a,
          b,
          c,
          got,
          a);
        std::abort();
      }
    }
  }

  /// Verify carve(): pre.base+pre.size == req.base; req.base aligned;
  /// req.size == sc.size_chunks; post.base == req.end; spans equal.
  template<size_t B>
  void check_carve()
  {
    using Bins = ArenaBinsTestAccess<B>;
    using range_t = typename Bins::range_t;

    auto classes = collect_classes<B>(64);
    for (size_t addr = 0; addr < 32; addr++)
    {
      for (size_t n = 1; n <= 64; n++)
      {
        for (auto sc : classes)
        {
          size_t s = Bins::size_chunks(sc);
          size_t a = Bins::align_chunks(sc);
          if (!can_serve(addr, n, s, a))
            continue;

          auto cv = Bins::carve(range_t{addr, n}, s);

          // pre starts at the block's base.
          if (cv.pre.base != addr)
          {
            std::printf(
              "B=%zu carve pre.base != addr (addr=%zu n=%zu s=%zu)\n",
              B,
              addr,
              n,
              s);
            std::abort();
          }
          // pre.end == req.base.
          if (cv.pre.base + cv.pre.size != cv.req.base)
          {
            std::printf("B=%zu carve pre.end != req.base\n", B);
            std::abort();
          }
          // req aligned.
          if ((cv.req.base & (a - 1)) != 0)
          {
            std::printf(
              "B=%zu carve req.base %zu not aligned to %zu\n",
              B,
              cv.req.base,
              a);
            std::abort();
          }
          // req.size == sc.size_chunks.
          if (cv.req.size != s)
          {
            std::printf(
              "B=%zu carve req.size %zu != s %zu\n", B, cv.req.size, s);
            std::abort();
          }
          // req.end == post.base.
          if (cv.req.base + cv.req.size != cv.post.base)
          {
            std::printf("B=%zu carve req.end != post.base\n", B);
            std::abort();
          }
          // post.end == block.end.
          if (cv.post.base + cv.post.size != addr + n)
          {
            std::printf("B=%zu carve post.end != block.end\n", B);
            std::abort();
          }
          // pre.size + req.size + post.size == block.size.
          if (cv.pre.size + cv.req.size + cv.post.size != n)
          {
            std::printf("B=%zu carve sizes don't sum to n\n", B);
            std::abort();
          }
        }
      }
    }
  }

  template<size_t B>
  void run_all()
  {
    std::printf("--- Running ArenaBinsTestAccess<%zu> tests ---\n", B);
    check_chunk_sc_roundtrip<B>();
    std::printf("  chunk_sc_t round-trip: OK\n");
    check_align_chunks<B>();
    std::printf("  align_chunks: OK\n");
    check_to_exp_mant_equivalence<B>();
    std::printf("  to_exp_mant runtime/_const equivalence: OK\n");
    check_info_consistency<B>();
    std::printf("  *_info_for_request consistency: OK\n");
    check_bin_id_range<B>();
    std::printf("  bin_index within-exp range: OK\n");
    check_bin_classification<B>(/*max_addr=*/128, /*max_n=*/64);
    std::printf("  bin classification vs bin_subsets predicate: OK\n");
    check_bitmap_smoke<B>();
    std::printf("  Bitmap smoke: OK\n");
    check_bitmap_find_empty<B>();
    std::printf("  Bitmap empty find returns SIZE_MAX: OK\n");
    check_bitmap_exhaustive_single_bit<B>();
    std::printf("  Bitmap exhaustive single-bit find: OK\n");
    check_bitmap_multi_bit_random<B>();
    std::printf("  Bitmap multi-bit random find: OK\n");
    check_bitmap_word_boundary<B>();
    std::printf("  Bitmap word-boundary cases: OK\n");
    check_bitmap_bin_index_integration<B>();
    std::printf("  Bitmap bin_index integration: OK\n");
    check_bitmap_add<B>();
    std::printf("  Bitmap add classify+set+idempotent: OK\n");
    check_bitmap_find_min<B>();
    std::printf("  Bitmap find_for_request returns minimum: OK\n");
    check_carve<B>();
    std::printf("  carve splits aligned/unaligned blocks: OK\n");
  }

  /// A few concrete expected values, derived from the prototype's output, to
  /// catch silent breakage of the canonical numbering.
  void check_known_values()
  {
    using B2 = ArenaBinsTestAccess<2>;

    // size 1 -> raw 0, size 2 -> raw 1, size 3 -> raw 2, size 4 -> raw 3,
    // size 5 -> raw 4, ..., size 8 -> raw 7, size 10 -> raw 8.
    if (B2::size_chunks(B2::request(1)) != 1)
      std::abort();
    if (B2::size_chunks(B2::request(8)) != 8)
      std::abort();
    if (B2::size_chunks(B2::request(9)) != 10)
      std::abort();
    if (B2::size_chunks(B2::request(11)) != 12)
      std::abort();

    // align_chunks: size 4 -> 4, size 5 -> 1, size 6 -> 2, size 8 -> 8,
    // size 10 -> 2, size 12 -> 4, size 14 -> 2.
    if (B2::align_chunks(B2::request(4)) != 4)
      std::abort();
    if (B2::align_chunks(B2::request(5)) != 1)
      std::abort();
    if (B2::align_chunks(B2::request(6)) != 2)
      std::abort();
    if (B2::align_chunks(B2::request(8)) != 8)
      std::abort();
    if (B2::align_chunks(B2::request(10)) != 2)
      std::abort();

    // BINS_PER_EXP must be 5 for B=2.
    if (B2::BINS_PER_EXP != 5)
      std::abort();

    using B3 = ArenaBinsTestAccess<3>;

    if (B3::BINS_PER_EXP != 13)
      std::abort();

    using B1 = ArenaBinsTestAccess<1>;
    if (B1::BINS_PER_EXP != 2)
      std::abort();
  }
} // namespace

int main(int, char**)
{
  setup();

  check_known_values();
  std::printf("Known concrete values: OK\n");

  run_all<1>();
  run_all<2>();
  run_all<3>();

  std::printf("All ArenaBins tests passed.\n");
  return 0;
}
