#include <iostream>
#include <snmalloc/snmalloc.h>

#ifndef SNMALLOC_PTHREAD_ATFORK_WORKS
int main()
{
  std::cout << "Test did not run" << std::endl;
  return 0;
}
#else

#  include <pthread.h>
#  include <thread>

void simulate_allocation()
{
  snmalloc::PreventFork pf;
}

int main()
{
  // Counter for the number of threads that are blocking the fork
  std::atomic<size_t> block = false;
  // Specifies that the forking thread has observed that all the blocking
  // threads are in place.
  std::atomic<bool> forking = false;

  size_t N = 3;

  snmalloc::message<1024>("Testing PreventFork");

  snmalloc::message<1024>("Adding alternative calls to pthread_atfork");
  pthread_atfork(simulate_allocation, simulate_allocation, simulate_allocation);

  snmalloc::message<1024>("Initialising PreventFork singleton");
  {
    // Cause initialisation of the PreventFork singleton to call pthread_atfork.
    snmalloc::PreventFork pf;
  }

  snmalloc::message<1024>("Adding alternative calls to pthread_atfork");
  pthread_atfork(simulate_allocation, simulate_allocation, simulate_allocation);

  snmalloc::message<1024>("Creating other threads");
  for (size_t i = 0; i < N; i++)
  {
    std::thread t([&block, &forking, i]() {
      {
        snmalloc::PreventFork pf;
        snmalloc::message<1024>("Thread {} blocking fork", i);
        block++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (!forking)
          std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        snmalloc::message<1024>("Thread {} releasing block", i);
        block--;
      }
    });

    t.detach();
  }

  snmalloc::message<1024>("Waiting for all threads to block fork");
  while (block != N)
    std::this_thread::yield();

  snmalloc::message<1024>("Forking");
  forking = true;
  fork();

  if (block)
  {
    snmalloc::message<1024>("PreventFork failed");
    return 1;
  }
  snmalloc::message<1024>("PreventFork passed");
  return 0;
}

#endif