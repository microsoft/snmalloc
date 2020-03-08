#pragma once

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// Needs to be included after windows.h
#  include <psapi.h>
#endif

#include <iomanip>
#include <iostream>

namespace usage
{
  void print_memory()
  {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;

    if (!GetProcessMemoryInfo(
          GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
      return;

    std::cout << "Memory info:" << std::endl
              << "\tPageFaultCount: " << pmc.PageFaultCount << std::endl
              << "\tPeakWorkingSetSize: " << pmc.PeakWorkingSetSize << std::endl
              << "\tWorkingSetSize: " << pmc.WorkingSetSize << std::endl
              << "\tQuotaPeakPagedPoolUsage: " << pmc.QuotaPeakPagedPoolUsage
              << std::endl
              << "\tQuotaPagedPoolUsage: " << pmc.QuotaPagedPoolUsage
              << std::endl
              << "\tQuotaPeakNonPagedPoolUsage: "
              << pmc.QuotaPeakNonPagedPoolUsage << std::endl
              << "\tQuotaNonPagedPoolUsage: " << pmc.QuotaNonPagedPoolUsage
              << std::endl
              << "\tPagefileUsage: " << pmc.PagefileUsage << std::endl
              << "\tPeakPagefileUsage: " << pmc.PeakPagefileUsage << std::endl
              << "\tPrivateUsage: " << pmc.PrivateUsage << std::endl;
#endif
  }
};
