/**
 * Memory usage test
 * Query memory usage repeatedly
 */

#include <iostream>
#include <test/setup.h>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#include "../../../override/malloc.cc"

using namespace snmalloc;

bool
print_memory_usage()
{
  static std::pair<size_t,size_t> last_memory_usage {0,0};

  auto next_memory_usage = default_memory_provider().memory_usage();
  if (next_memory_usage != last_memory_usage)
  {
    std::cout << "Memory Usages Changed to (" << next_memory_usage.first << ", " << next_memory_usage.second << ")" << std::endl;
    last_memory_usage = next_memory_usage;
    return true;
  }
  return false;
}

std::vector<void*> allocs;

/**
 * Add allocs until the statistics have changed n times.
 */
void add_n_allocs(size_t n)
{
  while (true)
  {
    allocs.push_back(our_malloc(1024));
    if (print_memory_usage())
    {
      n--;
      if (n == 0) break;
    }
  }
}

/**
 * Remove allocs until the statistics have changed n times.
 */
void remove_n_allocs(size_t n)
{
  while (true)
  {
    our_free(allocs.back());
    allocs.pop_back();
    if (print_memory_usage())
    {
      n--;
      if (n == 0) break;
    }
  }
}

int main(int argc, char** argv)
{
  UNUSED(argc);
  UNUSED(argv);

  setup();

  add_n_allocs(5);
  std::cout << "Init complete!" << std::endl;
  
  for (int i = 0; i < 10; i++)
  {
    remove_n_allocs(1);
    std::cout << "Phase " << i << " remove complete!" << std::endl;
    add_n_allocs(2);
    std::cout << "Phase " << i << " add complete!" << std::endl;
  }

  for (int i = 0; i < 10; i++)
  {
    remove_n_allocs(2);
    std::cout << "Phase " << i << " remove complete!" << std::endl;
    add_n_allocs(1);
    std::cout << "Phase " << i << " add complete!" << std::endl;
  }

  remove_n_allocs(3);
  std::cout << "Teardown complete!" << std::endl;
}
