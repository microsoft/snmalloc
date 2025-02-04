/**
 * This benchmark is based on 
 * https://github.com/microsoft/mimalloc/issues/1002#issuecomment-2630410617
 *
 * It causes large batchs of memory to be freed on a remote thread, and causes many
 * aspects of the backend to be under-contention.
 *
 * The benchmark has a single freeing thread, and many allocating threads. The allocating
 * threads communicate using a shared list of memory to free, which is protected by a mutex.
 * This causes interesting batch behaviour which triggered a bug in the linux backend.
 */
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
using namespace std;

#include <snmalloc/snmalloc.h>
#define malloc snmalloc::libc::malloc
#define free snmalloc::libc::free
#define malloc_usable_size snmalloc::libc::malloc_usable_size

std::mutex global_tofree_list_mtx;
std::vector<void *> global_tofree_list;

std::atomic_int mustexit;
void freeloop()
{
    size_t max_list_bytes = 0;
    while (1)
    {
        std::lock_guard<std::mutex> guard{global_tofree_list_mtx};
        size_t list_bytes = 0 ;
        for (auto & p : global_tofree_list)
        {
            list_bytes += malloc_usable_size(p);
            free(p);
        }
        global_tofree_list.clear();

        if (list_bytes > max_list_bytes)
        {
            printf("%zd bytes\n", list_bytes);
            max_list_bytes = list_bytes;
        }

        if (mustexit)
            return;
    }
}

void looper()
{
    std::vector<void *> tofree_list;
    auto flush = [&]()
    {
      {
        std::lock_guard<std::mutex> guard{global_tofree_list_mtx};
        for (auto & p : tofree_list)
            global_tofree_list.push_back(p);
      }
      tofree_list.clear();
    };

    auto do_free = [&](void * p)
    {
        tofree_list.push_back(p);
        if (tofree_list.size() > 100)
        {
          flush();
        }
    };

    for (int i = 0; i < 200000; ++i)
    {
        size_t s = 1ULL << (i%20);
        for (size_t j = 0; j < 8; j++)
        {
            auto ptr = (int *)malloc(s*sizeof(int));
            *ptr = 1523;
            do_free(ptr);
        }
    }

    flush();
    fflush(stdout);
}

int main()
{
    int threadcount = 8;
    vector<thread> threads;
    
    for (int i = 0; i < threadcount; ++i)
        threads.emplace_back(looper);
    
    std::thread freeloop_thread(freeloop);
    
    for (auto & thread : threads)
    {
        thread.join();
    }
    
    mustexit.store(1);
    freeloop_thread.join();
    
    puts("Done!");
    
    return 0;
}