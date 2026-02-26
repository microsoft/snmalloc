#include <iostream>
#include <queue>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

int main()
{
  std::vector<std::thread> threads;
  std::atomic<size_t> running;
  snmalloc::Stat requests;
  std::atomic<bool> done{false};

  for (size_t i = 0; i < 16; i++)
  {
    threads.push_back(std::thread([&running, &requests, &done]() {
      std::queue<size_t*> q;
      while (!done)
      {
        snmalloc::ScopedAllocator alloc;
        running++;

        if (rand() % 1000 == 0)
        {
          // Deallocate everything in the queue
          while (q.size() > 0)
          {
            auto p = q.front();
            requests -= *p;
            alloc->dealloc(p);
            q.pop();
          }
        }

        for (size_t j = 0; j < 1000; j++)
        {
          if (q.size() >= 20000 || (q.size() > 0 && (rand() % 10 == 0)))
          {
            auto p = q.front();
            requests -= *p;
            alloc->dealloc(p);
            q.pop();
          }
          else
          {
            size_t size =
              (rand() % 1024 == 0) ? 16 * 1024 * (1 << (rand() % 3)) : 48;
            requests += size;
            auto p = (size_t*)alloc->alloc(size);
            *p = size;
            q.push(p);
          }
        }

        running--;
        std::this_thread::sleep_for(std::chrono::microseconds(rand() % 2000));
      }
    }));
  }

  std::thread([&requests]() {
    size_t count = 0;
    while (count < 60)
    {
      count++;
      std::this_thread::sleep_for(std::chrono::seconds(1));
      // std::cout << "Inflight:            " <<
      // snmalloc::RemoteDeallocCache<snmalloc::Config>::remote_inflight <<
      // std::endl; std::cout
      // << "Current reservation: " << snmalloc::Globals::get_current_usage() <<
      // std::endl; std::cout << "Peak reservation:    " <<
      // snmalloc::Globals::get_peak_usage() << std::endl; std::cout <<
      // "Allocator count:     " << snmalloc::Globals::pool().get_count() <<
      // std::endl; std::cout << "Running threads:     " << running <<
      // std::endl; std::cout << "Index:               " << count << std::endl;
      // std::cout << "------------------------------------------" << std::endl;
      std::cout
        << count << "," << snmalloc::Alloc::Config::Backend::get_peak_usage()
        << "," << snmalloc::Alloc::Config::Backend::get_current_usage() << ","
        << requests.get_curr() << "," << requests.get_peak() << ","
        << snmalloc::RemoteDeallocCache<snmalloc::Config>::remote_inflight
             .get_peak()
        << ","
        << snmalloc::RemoteDeallocCache<snmalloc::Config>::remote_inflight
             .get_curr()
        << std::endl;
      snmalloc::print_alloc_stats();
    }
  }).join();

  done = true;

  for (auto& t : threads)
    t.join();

  return 0;
}