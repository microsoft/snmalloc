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

  for (size_t size_class = 0; size_class < NUM_SIZECLASSES; size_class++)
  {
    size_t rsize = sizeclass_to_size((uint8_t)size_class);
    for (size_t offset = 0; offset < SUPERSLAB_SIZE; offset++)
    {
      size_t rounded = (offset / rsize) * rsize;
      bool mod_0 = (offset % rsize) == 0;

      size_t opt_rounded = round_by_sizeclass(rsize, offset);
      if (rounded != opt_rounded)
        abort();

      bool opt_mod_0 = is_multiple_of_sizeclass(rsize, offset);
      if (opt_mod_0 != mod_0)
        abort();
    }
  }
  return 0;
}
