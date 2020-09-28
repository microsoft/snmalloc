#include <iostream>
#include <snmalloc.h>
#include <test/setup.h>

NOINLINE
snmalloc::sizeclass_t size_to_sizeclass(size_t size)
{
  return snmalloc::size_to_sizeclass(size);
}

void test_align_size()
{
  bool failed = false;

  SNMALLOC_CHECK(snmalloc::aligned_size(128, 160) == 256);

  for (size_t size = 1;
       size < snmalloc::sizeclass_to_size(snmalloc::NUM_SIZECLASSES - 1);
       size++)
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

    for (size_t alignment_bits = 0; alignment_bits < snmalloc::SUPERSLAB_BITS;
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

int main(int, char**)
{
  setup();

  bool failed = false;
  size_t size_low = 0;

  std::cout << "0 has sizeclass: " << (size_t)snmalloc::size_to_sizeclass(0)
            << std::endl;

  std::cout << "sizeclass |-> [size_low, size_high] " << std::endl;

  for (snmalloc::sizeclass_t sz = 0; sz < snmalloc::NUM_SIZECLASSES; sz++)
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

  test_align_size();
}