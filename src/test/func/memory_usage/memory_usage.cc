/**
 * Memory usage test
 * Query memory usage repeatedly
 */
#include <iostream>
#include <test/setup.h>
#include <vector>

#define SNMALLOC_NAME_MANGLE(a) our_##a
#include "../../../snmalloc/override/malloc-extensions.cc"
#include "../../../snmalloc/override/malloc.cc"

using namespace snmalloc;

bool print_memory_usage()
{
  static malloc_info_v1 last_memory_usage;
  malloc_info_v1 next_memory_usage;

  get_malloc_info_v1(&next_memory_usage);

  if (
    (next_memory_usage.current_memory_usage !=
     last_memory_usage.current_memory_usage) ||
    (next_memory_usage.peak_memory_usage !=
     last_memory_usage.peak_memory_usage))
  {
    std::cout << "Memory Usages Changed to ("
              << next_memory_usage.current_memory_usage << ", "
              << next_memory_usage.peak_memory_usage << ")" << std::endl;
    last_memory_usage = next_memory_usage;
    return true;
  }
  return false;
}

std::vector<void*> allocs{};

/**
 * Add allocs until the statistics have changed n times.
 */
void add_n_allocs(size_t n)
{
  while (true)
  {
    auto p = our_malloc(1024);
    allocs.push_back(p);
    if (print_memory_usage())
    {
      n--;
      if (n == 0)
        break;
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
    if (allocs.empty())
      return;
    auto p = allocs.back();
    our_free(p);
    allocs.pop_back();
    if (print_memory_usage())
    {
      n--;
      if (n == 0)
        break;
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
