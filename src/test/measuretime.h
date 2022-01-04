#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

class MeasureTime
{
  std::stringstream ss;
  std::chrono::time_point<std::chrono::high_resolution_clock> start =
    std::chrono::high_resolution_clock::now();

  bool quiet = false;

public:
  ~MeasureTime()
  {
    auto finish = std::chrono::high_resolution_clock::now();
    auto diff = finish - start;
    if (!quiet)
    {
      std::cout << ss.str() << ": " << std::setw(12) << diff.count() << " ns"
                << std::endl;
    }
  }

  MeasureTime(bool quiet = false) : quiet(quiet) {}

  template<typename T>
  MeasureTime& operator<<(const T& s)
  {
    ss << s;
    start = std::chrono::high_resolution_clock::now();
    return *this;
  }

  std::chrono::nanoseconds get_time()
  {
    auto finish = std::chrono::high_resolution_clock::now();
    auto diff = finish - start;
    return diff;
  }
};