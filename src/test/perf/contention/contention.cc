#include "test/opt.h"
#include "test/setup.h"
#include "test/usage.h"
#include "test/xoroshiro.h"

#include <iomanip>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

using namespace snmalloc;

bool use_malloc = false;

template<void f(size_t id)>
class ParallelTest
{
private:
  std::atomic<bool> flag = false;
  std::atomic<size_t> ready = 0;
  uint64_t start;
  uint64_t end;
  std::atomic<size_t> complete = 0;

  size_t cores;

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
  ParallelTest(size_t cores) : cores(cores)
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

std::atomic<size_t*>* contention;
size_t swapsize;
size_t swapcount;

void test_tasks_f(size_t id)
{
  xoroshiro::p128r32 r(id + 5000);

  for (size_t n = 0; n < swapcount; n++)
  {
    size_t size = 16 + (r.next() % 1024);
    size_t* res = (size_t*)(use_malloc ? malloc(size) : snmalloc::alloc(size));

    if (res != nullptr)
    {
      *res = size;
    }
    else
    {
      std::cout << "Failed to allocate " << size << " bytes" << std::endl;
      // Continue as this is not an important failure.
    }

    size_t* out =
      contention[n % swapsize].exchange(res, std::memory_order_acq_rel);

    if (out != nullptr)
    {
      size = *out;
      if (use_malloc)
        free(out);
      else
        snmalloc::dealloc(out, size);
    }
  }
};

void test_tasks(size_t num_tasks, size_t count, size_t size)
{
  std::cout << "Sequential setup" << std::endl;

  contention = new std::atomic<size_t*>[size];
  xoroshiro::p128r32 r;

  for (size_t n = 0; n < size; n++)
  {
    size_t alloc_size = 16 + (r.next() % 1024);
    size_t* res =
      (size_t*)(use_malloc ? malloc(alloc_size) : snmalloc::alloc(alloc_size));
    *res = alloc_size;
    contention[n] = res;
  }
  swapcount = count;
  swapsize = size;

#ifdef USE_SNMALLOC_STATS
  Stats s0;
  current_alloc_pool()->aggregate_stats(s0);
#endif
  std::cout << "Begin parallel test:" << std::endl;

  {
    ParallelTest<test_tasks_f> test(num_tasks);

    std::cout << "Task test, " << num_tasks << " threads, " << count
              << " swaps per thread " << test.time() << " ticks" << std::endl;

    for (size_t n = 0; n < swapsize; n++)
    {
      if (contention[n] != nullptr)
      {
        if (use_malloc)
          free(contention[n]);
        else
          snmalloc::dealloc(contention[n], *contention[n]);
      }
    }

    delete[] contention;
  }

#ifndef NDEBUG
  snmalloc::debug_check_empty();
#endif
};

int main(int argc, char** argv)
{
  setup();

  opt::Opt opt(argc, argv);
  size_t cores = opt.is<size_t>("--cores", 8);

  size_t count = opt.is<size_t>("--swapcount", 1 << 20);
  size_t size = opt.is<size_t>("--swapsize", 1 << 18);
  use_malloc = opt.has("--use_malloc");

  std::cout << "Allocator is " << (use_malloc ? "System" : "snmalloc")
            << std::endl;

  for (size_t i = cores; i > 0; i >>= 1)
    test_tasks(i, count, size);

  if (opt.has("--stats"))
  {
#ifdef USE_SNMALLOC_STATS
    Stats s;
    current_alloc_pool()->aggregate_stats(s);
    s.print<Alloc>(std::cout);
#endif

    usage::print_memory();
  }

  return 0;
}
