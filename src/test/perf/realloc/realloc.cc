#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <algorithm>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

using namespace snmalloc;

NOINLINE
void* myrealloc(void* p, size_t size)
{
  return snmalloc::libc::realloc(p, size);
}

void grow()
{
  void* base = nullptr;
  for (size_t i = 1; i < 1000; i++)
  {
    base = myrealloc(base, i * 8);
  }
  snmalloc::libc::free(base);
}

int main()
{
  auto start = Aal::tick();

  for (size_t i = 0; i < 10000; i++)
  {
    grow();
    if (i % 10 == 0)
    {
      std::cout << "." << std::flush;
    }
  }

  auto end = Aal::tick();

  std::cout << "Taken: " << end - start << std::endl;
}