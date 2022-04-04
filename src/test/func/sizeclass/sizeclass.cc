#include <iostream>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>

NOINLINE
snmalloc::smallsizeclass_t size_to_sizeclass(size_t size)
{
  return snmalloc::size_to_sizeclass(size);
}

void test_align_size()
{
  bool failed = false;

  SNMALLOC_CHECK(snmalloc::aligned_size(128, 160) == 256);

  for (size_t size = 1;
       size < snmalloc::sizeclass_to_size(snmalloc::NUM_SMALL_SIZECLASSES - 1);
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

int main(int, char**)
{
  setup();

  bool failed = false;
  size_t size_low = 0;

  std::cout << "0 has sizeclass: " << (size_t)snmalloc::size_to_sizeclass(0)
            << std::endl;

  std::cout << "sizeclass |-> [size_low, size_high] " << std::endl;

  size_t slab_size = 0;
  for (snmalloc::smallsizeclass_t sz = 0; sz < snmalloc::NUM_SMALL_SIZECLASSES;
       sz++)
  {
    if (
      sz < snmalloc::NUM_SMALL_SIZECLASSES &&
      slab_size != snmalloc::sizeclass_to_slab_size(sz))
    {
      slab_size = snmalloc::sizeclass_to_slab_size(sz);
      std::cout << std::endl;
    }

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
