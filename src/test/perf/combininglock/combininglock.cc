#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

snmalloc::CombiningLock cl;

std::atomic<bool> run{true};

void loop()
{
  size_t j = 0;
  size_t i = 0;
  while (run)
  {
    i++;
    snmalloc::with(cl, [&]() { j++; });
    if (i != j)
      snmalloc::error("i != j");
  }
}

int main()
{
  std::vector<std::thread> threads;
  for (size_t i = 0; i < 8; i++)
  {
    threads.emplace_back(std::thread(loop));
  }

  std::this_thread::sleep_for(std::chrono::seconds(100));
  run = false;

  for (auto& t : threads)
  {
    t.join();
  }
}