#include <iostream>
#include <snmalloc.h>

NOINLINE
uint8_t size_to_sizeclass(size_t size)
{
  return snmalloc::size_to_sizeclass(size);
}

int main(int, char**)
{
  bool failed = false;
  size_t size_low = 0;

  std::cout << "0 has sizeclass: " << (size_t)snmalloc::size_to_sizeclass(0)
            << std::endl;

  std::cout << "sizeclass |-> [size_low, size_high] " << std::endl;

  for (uint8_t sz = 0; sz < snmalloc::NUM_SIZECLASSES; sz++)
  {
    // Separate printing for small and medium sizeclasses
    if (sz == snmalloc::NUM_SMALL_CLASSES)
      std::cout << std::endl;

    size_t size = snmalloc::sizeclass_to_size(sz);
    std::cout << (size_t)sz << " |-> "
              << "[" << size_low + 1 << ", " << size << "]" << std::endl;

    if (size < size_low)
    {
      std::cout << "Sizeclass " << (size_t)sz << " is " << size
                << " which is less than " << size_low << std::endl;
      failed = true;
    }

    for (size_t i = size_low + 1; i <= size; i++)
    {
      if (size_to_sizeclass(i) != sz)
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
}