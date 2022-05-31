#pragma once

#include <atomic>

namespace snmalloc
{
  class PalTidDefault
  {
  public:
    using ThreadIdentity = size_t;

    /**
     * @brief Get the an id for the current thread.
     *
     * @return the thread id, this should never be the default of
     * ThreadIdentity. Callers can assume it is a non-default value.
     */
    static inline ThreadIdentity get_tid() noexcept
    {
      static thread_local size_t tid{0};
      static std::atomic<size_t> tid_source{0};

      if (tid == 0)
      {
        tid = ++tid_source;
      }
      return tid;
    }
  };
} // namespace snmalloc