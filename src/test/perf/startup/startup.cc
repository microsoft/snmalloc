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

std::vector<uint64_t> counters{};

template<typename F>
class ParallelTest
{
private:
  std::atomic<bool> flag = false;
  std::atomic<size_t> ready = 0;
  uint64_t start;
  uint64_t end;
  std::atomic<size_t> complete = 0;
  size_t cores;
  F f;

  void run(size_t id)
  {
    auto prev = ready.fetch_add(1);
    if (prev + 1 == cores)
    {
      start = Aal::tick();
      flag = true;
    }
    while (!flag)
      Aal::pause();

    f(id);

    prev = complete.fetch_add(1);
    if (prev + 1 == cores)
    {
      end = Aal::tick();
    }
  }

public:
  ParallelTest(F&& f, size_t cores) : cores(cores), f(std::forward<F>(f))
  {
    std::thread* t = new std::thread[cores];

    for (size_t i = 0; i < cores; i++)
    {
      t[i] = std::thread(&ParallelTest::run, this, i);
    }
    // Wait for all the threads.
    for (size_t i = 0; i < cores; i++)
    {
      t[i].join();
    }

    delete[] t;
  }

  uint64_t time()
  {
    return end - start;
  }
};

int main()
{
  auto nthreads = std::thread::hardware_concurrency();
  counters.resize(nthreads);

  ParallelTest test(
    [](size_t id) {
      auto start = Aal::tick();
      snmalloc::dealloc(snmalloc::alloc(1));
      auto end = Aal::tick();
      counters[id] = end - start;
    },
    nthreads);

  std::cout << "Taken: " << test.time() << std::endl;
  std::sort(counters.begin(), counters.end());
  uint64_t start = 0;
  for (auto counter : counters)
  {
    std::cout << "Thread time " << counter << " (" << counter - start << ")"
              << std::endl;
    start = counter;
  }
}