#include <snmalloc/snmalloc.h>

#include <iostream>
#include <thread>

void ecall()
{
    snmalloc::ScopedAllocator a;
    std::vector<void*> allocs;
    size_t count = 0;
    for (size_t j = 0; j < 1000; j++)
    {
        allocs.push_back(a.alloc.alloc(j % 1024));
        count += j % 1024;
    }
    auto p = a.alloc.alloc(1 * 1024 * 1024);
    memset(p, 0, 1 * 1024 * 1024);

    for (size_t j = 0; j < allocs.size(); j++)
        a.alloc.dealloc(allocs[j]);
    
    a.alloc.dealloc(p);
}

void thread_body()
{
  for (int i = 0; i < 10000; i++) {
    ecall();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

void monitor_body()
{
   for (int i = 0; i < 10000; i++) { 
    std::cout << "Current: " << snmalloc::Alloc::StateHandle::get_current_usage() << std::endl;
    std::cout << "Peak   : " << snmalloc::Alloc::StateHandle::get_peak_usage() << std::endl;
    std::cout << "Allocs : " << snmalloc::Alloc::StateHandle::pool().get_count() << std::endl;
    std::cout << "--------------------------------------------" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
   }
}

int main()
{
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++)
  {
    threads.push_back(std::thread(thread_body));
  }
  threads.push_back(std::thread(monitor_body));

  for (auto& t : threads)
    t.join();
  return 0;
}