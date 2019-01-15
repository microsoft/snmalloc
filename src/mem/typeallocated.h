#pragma once

#include "../ds/bits.h"

namespace snmalloc
{
  template<class T>
  class TypeAllocated
  {
  private:
    template<class TT, class MemoryProvider>
    friend class TypeAlloc;
    template<class TT, Construction c>
    friend class MPMCStack;

    std::atomic<T*> next = nullptr;
    T* list_next;
  };
}