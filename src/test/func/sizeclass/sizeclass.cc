#include <iostream>
#include <snmalloc/snmalloc_core.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>

NOINLINE
snmalloc::smallsizeclass_t size_to_sizeclass(size_t size)
{
  return snmalloc::size_to_sizeclass(size);
}

static constexpr snmalloc::smallsizeclass_t minimum_sizeclass =
  snmalloc::size_to_sizeclass_const(snmalloc::MIN_ALLOC_SIZE);

void test_align_size()
{
  bool failed = false;

  SNMALLOC_CHECK(snmalloc::aligned_size(128, 160) == 256);

  for (size_t size = 1; snmalloc::is_small_sizeclass(size); size++)
  {
    size_t rsize = snmalloc::round_size(size);

    if (rsize < size)
    {
      std::cout << "Size class rounding shrunk: " << size << " -> " << rsize
                << std::endl;
      failed |= true;
    }

    auto lsb_rsize = rsize & (~rsize + 1);
    auto lsb_size = size & (~size + 1);

    if (lsb_rsize < lsb_size)
    {
      std::cout << "Original size more aligned than rounded size: " << size
                << " (" << lsb_size << ") -> " << rsize << " (" << lsb_rsize
                << ")" << std::endl;
      failed |= true;
    }

    for (size_t alignment_bits = 0;
         alignment_bits < snmalloc::MAX_SMALL_SIZECLASS_BITS;
         alignment_bits++)
    {
      auto alignment = (size_t)1 << alignment_bits;
      auto asize = snmalloc::aligned_size(alignment, size);

      if (asize < size)
      {
        std::cout << "Shrunk! Alignment: " << alignment << " Size: " << size
                  << " ASize: " << asize << std::endl;
        failed |= true;
      }

      if ((asize & (alignment - 1)) != 0)
      {
        std::cout << "Not aligned! Alignment: " << alignment
                  << " Size: " << size << " ASize: " << asize << std::endl;
        failed |= true;
      }
    }
  }

  if (failed)
    abort();
}

void test_uniform_large_sizeclasses()
{
  using namespace snmalloc;
  bool failed = false;

  // Sentinel sanity: default-constructed sizeclass_t is the unmapped sentinel
  // and not classified as small.
  if (sizeclass_t{}.raw() != 0)
  {
    std::cout << "Default sizeclass_t raw is " << sizeclass_t{}.raw()
              << " expected 0" << std::endl;
    failed = true;
  }
  if (sizeclass_t{}.is_default() != true)
  {
    std::cout << "Default sizeclass_t .is_default() is false" << std::endl;
    failed = true;
  }
  if (sizeclass_t{}.is_small())
  {
    std::cout << "Default sizeclass_t.is_small() is true" << std::endl;
    failed = true;
  }

  // Encoding sanity: small range and large range are disjoint and adjacent
  // in the value space.
  if (sizeclass_t::from_small_class(smallsizeclass_t(0)).raw() != 1)
  {
    std::cout << "from_small_class(0).raw() != 1" << std::endl;
    failed = true;
  }
  if (
    sizeclass_t::from_small_class(smallsizeclass_t(NUM_SMALL_SIZECLASSES - 1))
        .raw() +
      1 !=
    sizeclass_t::from_large_class(0).raw())
  {
    std::cout << "Small/large ranges are not adjacent" << std::endl;
    failed = true;
  }
  if (
    sizeclass_t::from_large_class(NUM_LARGE_CLASSES - 1).raw() >=
    SIZECLASS_REP_SIZE)
  {
    std::cout << "Largest large sizeclass overflows SIZECLASS_REP_SIZE"
              << std::endl;
    failed = true;
  }
  if (!sizeclass_t::from_small_class(smallsizeclass_t(0)).is_small())
  {
    std::cout << "from_small_class(0).is_small() is false" << std::endl;
    failed = true;
  }
  if (sizeclass_t::from_large_class(0).is_small())
  {
    std::cout << "from_large_class(0).is_small() is true" << std::endl;
    failed = true;
  }

  // Large sizeclasses are strictly increasing in size with lc.
  size_t prev_size = 0;
  for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
  {
    size_t size = sizeclass_full_to_size(sizeclass_t::from_large_class(lc));
    if (size <= prev_size)
    {
      std::cout << "Non-monotonic large sizeclass: lc=" << lc
                << " size=" << size << " prev=" << prev_size << std::endl;
      failed = true;
    }
    prev_size = size;
  }

  // Round-trip identity on pow2 large sizes: every pow2 size S in
  // [MAX_SMALL_SIZECLASS_SIZE * 2, MAX_LARGE_SIZECLASS_SIZE] must
  // satisfy sizeclass_full_to_size(size_to_sizeclass_full(S)) == S.
  // Bound the loop by ENCODED_ADDRESS_BITS so `bits::one_at_bit(b)`
  // never shifts by >= BITS (the bound check itself would fail on
  // 32-bit otherwise).
  for (size_t b = MAX_SMALL_SIZECLASS_BITS + 1; b <= ENCODED_ADDRESS_BITS; b++)
  {
    size_t S = bits::one_at_bit(b);
    sizeclass_t sc = size_to_sizeclass_full(S);
    size_t rs = sizeclass_full_to_size(sc);
    if (rs != S)
    {
      std::cout << "Pow2 round-trip failed: S=" << S << " round=" << rs
                << std::endl;
      failed = true;
    }

    // For every non-pow2 size X strictly between adjacent pow2 [P, 2P),
    // `size_to_sizeclass_full(X)` must select the smallest sizeclass
    // whose size is >= X. Compute the expected sizeclass independently
    // by scanning all large classes. Only check when 2P is still
    // representable.
    if (b < ENCODED_ADDRESS_BITS)
    {
      size_t mid = S + (S >> 1);
      sizeclass_t sc_mid = size_to_sizeclass_full(mid);
      size_t rs_mid = sizeclass_full_to_size(sc_mid);

      // Independent computation: smallest large class size >= mid.
      size_t expect = 0;
      for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
      {
        size_t sz = sizeclass_full_to_size(sizeclass_t::from_large_class(lc));
        if (sz >= mid)
        {
          expect = sz;
          break;
        }
      }
      if (expect == 0)
      {
        std::cout << "No large class >= mid=" << mid << std::endl;
        failed = true;
      }
      else if (rs_mid != expect)
      {
        std::cout << "Non-pow2 should round to smallest enclosing class: X="
                  << mid << " round=" << rs_mid << " expected=" << expect
                  << std::endl;
        failed = true;
      }
    }
  }

  // `round_size` contract: for every representable large class size
  // S, `round_size(S) == S` and `round_size(S_prev + 1) == S` (the
  // smallest enclosing class). `DefaultConts::success` (corealloc.h)
  // uses `round_size` to size the `calloc` zeroing range, so any
  // drift here would over- or under-zero. This is the deterministic
  // gate for that contract; the `calloc` smoke test in `memory.cc`
  // would not necessarily fault on an overshoot into backend free
  // range.
  {
    size_t prev = 0;
    for (size_t lc = 0; lc < NUM_LARGE_CLASSES; lc++)
    {
      size_t S = sizeclass_full_to_size(sizeclass_t::from_large_class(lc));
      if (round_size(S) != S)
      {
        std::cout << "round_size identity failed at large class: S=" << S
                  << " round_size=" << round_size(S) << std::endl;
        failed = true;
      }
      if (prev != 0 && prev + 1 < S)
      {
        size_t probe = prev + 1;
        if (round_size(probe) != S)
        {
          std::cout << "round_size(prev+1) blow-up: probe=" << probe
                    << " round_size=" << round_size(probe) << " expected=" << S
                    << std::endl;
          failed = true;
        }
      }
      prev = S;
    }
  }

  if (failed)
    abort();
}

int main(int, char**)
{
  setup();

  bool failed = false;
  size_t size_low = 0;

  std::cout << "Configured with minimum allocation size "
            << snmalloc::MIN_ALLOC_SIZE << " and step size "
            << snmalloc::MIN_ALLOC_STEP_SIZE << std::endl;

  std::cout << "0 has sizeclass: " << (size_t)snmalloc::size_to_sizeclass(0)
            << std::endl;

  std::cout << "sizeclass |-> [size_low, size_high] " << std::endl;

  size_t slab_size = 0;
  for (snmalloc::smallsizeclass_t sz(0); sz < snmalloc::NUM_SMALL_SIZECLASSES;
       sz++)
  {
    if (
      sz < snmalloc::NUM_SMALL_SIZECLASSES &&
      slab_size != snmalloc::sizeclass_to_slab_size(sz))
    {
      slab_size = snmalloc::sizeclass_to_slab_size(sz);
      std::cout << std::endl << "slab size: " << slab_size << std::endl;
    }

    size_t size = snmalloc::sizeclass_to_size(sz);
    std::cout << (size_t)sz << " |-> "
              << "[" << size_low + 1 << ", " << size << "]"
              << (sz == minimum_sizeclass ? " is minimum class" : "")
              << std::endl;

    if (size < size_low)
    {
      std::cout << "Sizeclass " << (size_t)sz << " is " << size
                << " which is less than " << size_low << std::endl;
      failed = true;
    }

    for (size_t i = size_low + 1; i <= size; i++)
    {
      /* All sizes should, via bit-math, come back to their class value */
      if (snmalloc::size_to_sizeclass_const(i) != sz)
      {
        std::cout << "Size " << i << " has _const sizeclass "
                  << (size_t)snmalloc::size_to_sizeclass_const(i)
                  << " but expected sizeclass " << (size_t)sz << std::endl;
        failed = true;
      }

      if (size < snmalloc::MIN_ALLOC_SIZE)
      {
        /*
         * It is expected that these sizes have the "wrong" class from tabular
         * lookup: they will have been clipped up to the minimum class.
         */
        if (size_to_sizeclass(i) != minimum_sizeclass)
        {
          std::cout << "Size " << i << " below minimum size; sizeclass "
                    << (size_t)size_to_sizeclass(i) << " not expected minimum "
                    << (size_t)minimum_sizeclass << std::endl;
          failed = true;
        }
      }
      else if (size_to_sizeclass(i) != sz)
      {
        std::cout << "Size " << i << " has sizeclass "
                  << (size_t)size_to_sizeclass(i) << " but expected sizeclass "
                  << (size_t)sz << std::endl;
        failed = true;
      }
    }

    size_low = size;
  }

  if (failed)
    abort();

  test_align_size();
  test_uniform_large_sizeclasses();
}
