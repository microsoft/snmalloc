/**
 * Unit tests for operations in bits.h
 */

#include <iostream>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>

void test_ctz()
{
  for (size_t i = 0; i < sizeof(size_t) * 8; i++)
    if (snmalloc::bits::ctz(snmalloc::bits::one_at_bit(i)) != i)
    {
      std::cout << "Failed with ctz(one_at_bit(i)) != i for i=" << i
                << std::endl;
      abort();
    }
}

void test_clz()
{
  const size_t PTRSIZE_LOG = sizeof(size_t) * 8;

  for (size_t i = 0; i < sizeof(size_t) * 8; i++)
    if (
      snmalloc::bits::clz(snmalloc::bits::one_at_bit(i)) !=
      (PTRSIZE_LOG - i - 1))
    {
      std::cout
        << "Failed with clz(one_at_bit(i)) != (PTRSIZE_LOG - i - 1) for i=" << i
        << std::endl;
      abort();
    }
}

int main(int argc, char** argv)
{
  snmalloc::UNUSED(argc, argv);

  setup();

  test_clz();
  test_ctz();
}
