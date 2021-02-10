
#pragma once

#include "pal_consts.h"
#include "pal_ds.h"
#include <chrono>

namespace snmalloc
{
  class PalTimerDefaultImpl
  {
    inline static PalTimer timers{};

  public:
    static uint64_t time_in_ms()
    {
      auto time = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

      // Process timers
      timers.check(time);

      return time;
    }

    static void register_timer(PalTimerObject* timer)
    {
      timers.register_timer(timer);
    }
  };
}