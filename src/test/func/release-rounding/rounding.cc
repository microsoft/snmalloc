#include <iostream>
#include <snmalloc.h>
#include <test/setup.h>
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

  for (size_t size_class = 0; size_class < NUM_SIZECLASSES; size_class++)
  {
    size_t rsize = sizeclass_to_size((uint8_t)size_class);
    size_t max_offset = sizeclass_to_slab_size(size_class);
    for (size_t offset = 0; offset < max_offset; offset++)
    {
      size_t rounded = (offset / rsize) * rsize;
      bool mod_0 = (offset % rsize) == 0;

      size_t opt_rounded = round_by_sizeclass(size_class, offset);
      if (rounded != opt_rounded)
      {
        std::cout << "rsize " << rsize << "  offset  " << offset << "  opt "
                  << opt_rounded << " correct " << rounded << std::endl
                  << std::flush;
        failed = true;
      }

      bool opt_mod_0 = is_multiple_of_sizeclass(size_class, offset);
      if (opt_mod_0 != mod_0)
      {
        std::cout << "rsize " << rsize << "  offset  " << offset << "  opt_mod "
                  << opt_mod_0 << " correct " << mod_0 << std::endl
                  << std::flush;
        failed = true;
      }
    }
  }
  if (failed)
    abort();
  return 0;
}
