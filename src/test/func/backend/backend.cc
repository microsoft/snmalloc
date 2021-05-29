#include "backend/backend.h"

#include "backend/slaballocator.h"
#include "mem/fastalloc.h"
#include "mem/pool.h"
#include "mem/threadalloc.h"
#include <ds/defines.h>
#include <iostream>


int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  // Test freeing nullptr, before any allocations.
  Alloc alloc_evil;
  alloc_evil.dealloc(nullptr);

  for (size_t i = 0; i < 44; i++)
  {
    Alloc& alloc = *(snmalloc::ThreadAlloc::get());
    Alloc alloc2;
    std::cout << "sizeclass: " << i << std::endl;

    for (size_t j = 0; j < 100; j++)
    {
      auto a = alloc.alloc(snmalloc::sizeclass_to_size(i));
      std::cout << "alloc " << j << ": " << a << std::endl;
      alloc2.dealloc(a);
    }
    std::cout << "-----------------------------------------------" << std::endl;
    alloc.flush();
    alloc2.flush();
  }
  return 0;
}
