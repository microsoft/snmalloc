#pragma once

#include "../ds/ds.h"
#include "backend_concept.h"

namespace snmalloc
{
  template<class T>
  class PoolState;

  template<class T>
  class Pooled
  {
  public:
    template<
      typename TT,
      SNMALLOC_CONCEPT(IsConfig) Config,
      PoolState<TT>& get_state()>
    friend class Pool;
    template<class a, Construction c>
    friend class MPMCStack;

    /// Used by the pool for chaining together entries when not in use.
    std::atomic<T*> next{nullptr};
    /// Used by the pool to keep the list of all entries ever created.
    capptr::Alloc<T> list_next;
    std::atomic<bool> in_use{false};

  public:
    void set_in_use()
    {
      if (in_use.exchange(true))
        error("Critical error: double use of Pooled Type!");
    }

    void reset_in_use()
    {
      in_use.store(false);
    }

    bool debug_is_in_use()
    {
      bool result = in_use.exchange(true);
      if (!result)
        in_use.store(false);
      return result;
    }
  };
} // namespace snmalloc
