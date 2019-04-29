#pragma once

#include "../ds/bits.h"

namespace snmalloc
{
  template<class T>
  class Pooled
  {
  private:
    template<class TT, class MemoryProvider>
    friend class Pool;
    template<class TT, Construction c>
    friend class MPMCStack;

    /// Used by the pool for chaining together entries when not in use.
    std::atomic<T*> next = nullptr;
    /// Used by the pool to keep the list of all entries ever created.
    T* list_next;
  };
} // namespace snmalloc