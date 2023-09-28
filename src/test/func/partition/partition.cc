#include <iostream>
#include <snmalloc/snmalloc.h>

struct OtherPartition{};

int main()
{
    std::vector<void*> allocs;
    
    for (size_t i = 0; i < 100; i++)
    {
        allocs.push_back(snmalloc::libc::malloc<OtherPartition>(1));
        std::cout << "Allocated " << allocs.back() << std::endl;
    }

    for (size_t i = 0; i < 100; i++)
    {
        allocs.push_back(snmalloc::libc::malloc<snmalloc::MainPartition>(1));
        std::cout << "Allocated " << allocs.back() << std::endl;
    }

    for (auto p : allocs)
    {
        snmalloc::libc::free(p);
    }
}
