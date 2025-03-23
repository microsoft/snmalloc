#include "snmalloc/snmalloc.h"

#include <iostream>

void test_step()
{
  auto b = snmalloc::get_scoped_allocator();
  auto a = snmalloc::get_scoped_allocator();

  for (size_t j = 0; j < 32; j++)
    for (size_t i = 0; i < 20; i++)
    {
      auto p = a->alloc(snmalloc::bits::one_at_bit(i));
      if (p != nullptr)
        b->dealloc(p);
      p = b->alloc(snmalloc::bits::one_at_bit(i));
      if (p != nullptr)
        a->dealloc(p);
    }
}

int main()
{
  for (size_t i = 0; i < 1000; i++)
  {
    if (i % 100 == 0)
    {
      std::cout << "Step " << i << std::endl;
      snmalloc::print_alloc_stats();
    }
    test_step();
  }
}