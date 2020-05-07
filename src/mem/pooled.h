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
    std::atomic<T*> next{nullptr};
    /// Used by the pool to keep the list of all entries ever created.
    T* list_next;
    std::atomic_flag in_use = ATOMIC_FLAG_INIT;

  public:
    void set_in_use()
    {
      if (in_use.test_and_set())
        error("Critical error: double use of Pooled Type!");
    }

    void reset_in_use()
    {
      in_use.clear();
    }

    bool debug_is_in_use()
    {
      bool result = in_use.test_and_set();
      if (!result)
        in_use.clear();
      return result;
    }
  };
} // namespace snmalloc