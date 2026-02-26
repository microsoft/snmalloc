#include <atomic>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

size_t threads{0};
size_t memory{0};
size_t iterations{0};

// Global barrier for synchronising threads.
std::atomic<size_t> barrier{0};
std::atomic<size_t> incarnation{0};

std::atomic<bool> stop{false};

std::vector<std::vector<void*>> allocations;

NOINLINE bool wait()
{
  auto old_incarnation = incarnation.load();
  // Register we have arrived at the barrier.
  if (--barrier == 0)
  {
    printf(".");
    fflush(stdout);
    barrier = threads;
    incarnation++;
    return stop;
  }

  while (incarnation.load() == old_incarnation)
  {
    if (stop)
      return true;
    snmalloc::Aal::pause();
  }

  return stop;
}

void thread_func(size_t tid)
{
  size_t size = 4097;
  size_t mem = memory / size;
  for (size_t j = 0; j < iterations; j++)
  {
    if (wait())
      return;
    std::vector<void*>& allocs = allocations[tid];
    for (size_t i = 0; i < mem; i++)
    {
      allocs.push_back(snmalloc::alloc(4097));
    }
    if (wait())
      return;
    std::vector<void*>& deallocs = allocations[(tid + 1) % threads];
    for (auto p : deallocs)
    {
      snmalloc::dealloc(p);
    }
    deallocs.clear();
  }
}

int main()
{
  threads = std::thread::hardware_concurrency();
  barrier = threads;

  if (snmalloc::DefaultPal::address_bits == 32)
    memory = snmalloc::bits::one_at_bit(30) / threads;
  else
    memory = snmalloc::bits::one_at_bit(32) / threads;
  iterations = 1000;

  for (size_t i = 0; i < threads; i++)
    allocations.emplace_back();

  std::vector<std::thread> thread_pool;
  for (size_t i = 0; i < threads; i++)
    thread_pool.emplace_back(thread_func, i);

  for (size_t i = 0; i < 30; i++)
  {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    snmalloc::print_alloc_stats();
  }
  stop = true;

  for (auto& t : thread_pool)
    t.join();
}
