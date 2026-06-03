#include <iostream>
#include <snmalloc/snmalloc_core.h>
#include <test/setup.h>
#include <test/snmalloc_testlib.h>
using namespace snmalloc;

// Check for all sizeclass that we correctly round every offset within
// a superslab to the correct value, by comparing with the standard
// unoptimised version using division.
// Also check we correctly determine multiples using optimized check.

int main(int argc, char** argv)
{
  setup();

  UNUSED(argc);
  UNUSED(argv);

  bool failed = false;

  // Layout invariant: osc(sc, off).raw() == sc.raw() | (off << SIZECLASS_BITS),
  // and the accessors invert that layout. This is load-bearing because
  // `SizeClassTable::start(sizeclass_t)` and `start(offset_and_sizeclass_t)`
  // both index by `.raw()`, so an offset=0 osc must hit the same table
  // row as the bare sizeclass_t; the offset>0 row-population loop in
  // the SizeClassTable ctor relies on the same layout. If any of this
  // drifts, `encode()` in metadata.h would silently produce wrong bits.
  for (smallsizeclass_t sc_small; sc_small < NUM_SMALL_SIZECLASSES; sc_small++)
  {
    sizeclass_t sc = sizeclass_t::from_small_class(sc_small);
    for (size_t off = 0; off < (size_t{1} << OFFSET_BITS); off++)
    {
      auto osc = offset_and_sizeclass_t(sc, off);
      size_t expected_raw = sc.raw() | (off << SIZECLASS_BITS);
      if (
        osc.raw() != expected_raw || osc.sizeclass() != sc ||
        osc.offset() != off)
      {
        std::cout << "osc layout mismatch: sc=" << sc.raw() << " off=" << off
                  << " -> raw=" << osc.raw() << " expected_raw=" << expected_raw
                  << " sc'=" << osc.sizeclass().raw()
                  << " off'=" << osc.offset() << std::endl
                  << std::flush;
        failed = true;
      }
    }
  }
  if (failed)
    abort();

  for (smallsizeclass_t size_class; size_class < NUM_SMALL_SIZECLASSES;
       size_class++)
  {
    size_t rsize = sizeclass_to_size(size_class);
    size_t max_offset = sizeclass_to_slab_size(size_class);
    sizeclass_t sc = sizeclass_t::from_small_class(size_class);
    offset_and_sizeclass_t osc = offset_and_sizeclass_t(sc, 0);
    for (size_t offset = 0; offset < max_offset; offset++)
    {
      size_t mod = offset % rsize;
      bool mod_0 = (offset % rsize) == 0;

      size_t opt_mod = index_in_object(osc, offset);
      if (mod != opt_mod)
      {
        std::cout << "rsize " << rsize << "  offset  " << offset << "  opt "
                  << opt_mod << " correct " << mod << std::endl
                  << std::flush;
        failed = true;
      }

      bool opt_mod_0 = is_start_of_object(osc, offset);
      if (opt_mod_0 != mod_0)
      {
        std::cout << "rsize " << rsize << "  offset  " << offset
                  << "  opt_mod0 " << opt_mod_0 << " correct " << mod_0
                  << std::endl
                  << std::flush;
        failed = true;
      }
    }
    if (failed)
      abort();
  }

  // Exercise pow2 large sizeclasses end-to-end.
  // For each pow2 size S that the front end actually reaches (lc values that
  // are pow2-aligned in the global exp+mantissa scheme), verify
  // index_in_object / is_start_of_object at a representative set of offsets:
  // the start of an object, an arbitrary interior offset, and the start of
  // the next object. Bound the loop by ENCODED_ADDRESS_BITS so
  // `bits::one_at_bit(b)` never shifts by >= BITS.
  for (size_t b = MAX_SMALL_SIZECLASS_BITS + 1; b <= ENCODED_ADDRESS_BITS; b++)
  {
    size_t S = bits::one_at_bit(b);
    sizeclass_t sc = size_to_sizeclass_full(S);
    offset_and_sizeclass_t osc = offset_and_sizeclass_t(sc, 0);

    address_t base = address_t(0);
    size_t offsets[] = {0, 1, S / 2, S - 1, S};
    for (size_t off : offsets)
    {
      address_t addr = base + off;
      size_t expected_mod = off % S;
      bool expected_start = expected_mod == 0;

      size_t opt_mod = index_in_object(osc, addr);
      if (opt_mod != expected_mod)
      {
        std::cout << "Large S=" << S << " offset=" << off
                  << " index_in_object=" << opt_mod
                  << " expected=" << expected_mod << std::endl;
        failed = true;
      }

      bool opt_start = is_start_of_object(osc, addr);
      if (opt_start != expected_start)
      {
        std::cout << "Large S=" << S << " offset=" << off
                  << " is_start_of_object=" << opt_start
                  << " expected=" << expected_start << std::endl;
        failed = true;
      }
    }
    if (failed)
      abort();
  }
  return 0;
}
