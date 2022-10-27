#ifdef SNMALLOC_PASS_THROUGH // This test depends on snmalloc internals
int main()
{
  return 0;
}
#else
#  include <iostream>
#  include <snmalloc/snmalloc.h>
#  include <vector>

template<size_t size>
void debug_check_empty_1()
{
  snmalloc::Alloc& a = snmalloc::ThreadAlloc::get();
  bool result;

  auto r = a.alloc(size);

  snmalloc::debug_check_empty<snmalloc::StandardConfig>(&result);
  if (result != false)
  {
    std::cout << "debug_check_empty failed to detect leaked memory:" << size
              << std::endl;
    abort();
  }

  a.dealloc(r);

  snmalloc::debug_check_empty<snmalloc::StandardConfig>(&result);
  if (result != true)
  {
    std::cout << "debug_check_empty failed to say empty:" << size << std::endl;
    abort();
  }

  r = a.alloc(size);

  snmalloc::debug_check_empty<snmalloc::StandardConfig>(&result);
  if (result != false)
  {
    std::cout << "debug_check_empty failed to detect leaked memory:" << size
              << std::endl;
    abort();
  }

  a.dealloc(r);

  snmalloc::debug_check_empty<snmalloc::StandardConfig>(&result);
  if (result != true)
  {
    std::cout << "debug_check_empty failed to say empty:" << size << std::endl;
    abort();
  }
}

template<size_t size>
void debug_check_empty_2()
{
  snmalloc::Alloc& a = snmalloc::ThreadAlloc::get();
  bool result;
  std::vector<void*> allocs;
  size_t count = 2048;

  for (size_t i = 0; i < count; i++)
  {
    auto r = a.alloc(128);
    allocs.push_back(r);
    snmalloc::debug_check_empty<snmalloc::StandardConfig>(&result);
    if (result != false)
    {
      std::cout << "False empty after " << i << " allocations of " << size
                << std::endl;
      abort();
    }
  }

  for (size_t i = 0; i < count; i++)
  {
    a.dealloc(allocs[i]);
  }
  snmalloc::debug_check_empty<snmalloc::StandardConfig>();
}

int main()
{
  debug_check_empty_1<16>();
  debug_check_empty_1<1024 * 1024 * 32>();

  debug_check_empty_2<32>();
  debug_check_empty_2<1024 * 1024 * 32>();
  return 0;
}
#endif