#include <iostream>
#include <snmalloc/snmalloc.h>
#include <vector>

template<size_t size>
void debug_check_empty_1()
{
  std::cout << "debug_check_empty_1 " << size << std::endl;
  bool result;

  auto r = snmalloc::alloc(size);

  snmalloc::debug_check_empty(&result);
  if (result != false)
  {
    std::cout << "debug_check_empty failed to detect leaked memory:" << size
              << std::endl;
    abort();
  }

  snmalloc::dealloc(r);

  snmalloc::debug_check_empty(&result);
  if (result != true)
  {
    std::cout << "debug_check_empty failed to say empty:" << size << std::endl;
    abort();
  }

  r = snmalloc::alloc(size);

  snmalloc::debug_check_empty(&result);
  if (result != false)
  {
    std::cout << "debug_check_empty failed to detect leaked memory:" << size
              << std::endl;
    abort();
  }

  snmalloc::dealloc(r);

  snmalloc::debug_check_empty(&result);
  if (result != true)
  {
    std::cout << "debug_check_empty failed to say empty:" << size << std::endl;
    abort();
  }
}

template<size_t size>
void debug_check_empty_2()
{
  std::cout << "debug_check_empty_2 " << size << std::endl;
  bool result;
  std::vector<void*> allocs;
  // 1GB of allocations
  size_t count = snmalloc::bits::min<size_t>(2048, 1024 * 1024 * 1024 / size);

  for (size_t i = 0; i < count; i++)
  {
    if (i % (count / 16) == 0)
    {
      std::cout << "." << std::flush;
    }
    auto r = snmalloc::alloc(size);
    allocs.push_back(r);
    snmalloc::debug_check_empty(&result);
    if (result != false)
    {
      std::cout << "False empty after " << i << " allocations of " << size
                << std::endl;
      abort();
    }
  }
  std::cout << std::endl;

  for (size_t i = 0; i < count; i++)
  {
    if (i % (count / 16) == 0)
    {
      std::cout << "." << std::flush;
    }
    snmalloc::debug_check_empty(&result);
    if (result != false)
    {
      std::cout << "False empty after " << i << " deallocations of " << size
                << std::endl;
      abort();
    }
    snmalloc::dealloc(allocs[i]);
  }
  std::cout << std::endl;
  snmalloc::debug_check_empty();
}

int main()
{
  debug_check_empty_1<16>();
  debug_check_empty_1<16384>();
  debug_check_empty_1<65536>();
  debug_check_empty_1<1024 * 1024 * 32>();

  debug_check_empty_2<32>();
  debug_check_empty_2<16384>();
  debug_check_empty_2<65535>();
  debug_check_empty_2<1024 * 1024 * 32>();

  return 0;
}
