/**
 * Unit tests for operations in pagemap operations.
 *
 * Currently this tests a very specific case where the pagemap
 * requires multiple levels of index. This was incorrectly implemented,
 * but no examples were using multiple levels of pagemap.
 */

#include <ds/bits.h>
#include <iostream>
#include <snmalloc.h>
#include <test/setup.h>

using namespace snmalloc;
using T = size_t;
static constexpr size_t GRANULARITY_BITS = 9;
static constexpr T PRIME = 251;
Pagemap<GRANULARITY_BITS, T, 0> pagemap_test;

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  T value = 0;
  for (uintptr_t ptr = 0; ptr < bits::one_at_bit(36);
       ptr += bits::one_at_bit(GRANULARITY_BITS + 3))
  {
    pagemap_test.set(ptr, value);
    value++;
    if (value == PRIME)
      value = 0;
    if ((ptr % (1ULL << 32)) == 0)
      std::cout << "." << std::flush;
  }

  std::cout << std::endl;
  value = 0;
  for (uintptr_t ptr = 0; ptr < bits::one_at_bit(36);
       ptr += bits::one_at_bit(GRANULARITY_BITS + 3))
  {
    T result = pagemap_test.get(ptr);
    if (value != result)
      Pal::error("Pagemap corrupt!");
    value++;
    if (value == PRIME)
      value = 0;

    if ((ptr % (1ULL << 32)) == 0)
      std::cout << "." << std::flush;
  }
  std::cout << std::endl;
}
