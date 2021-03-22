#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

class MeasureTime : public std::stringstream
{
  std::chrono::time_point<std::chrono::high_resolution_clock> start =
    std::chrono::high_resolution_clock::now();

public:
  ~MeasureTime()
  {
    auto finish = std::chrono::high_resolution_clock::now();
    auto diff = finish - start;
    std::cout << str() << ": " << std::setw(12) << diff.count() << " ns"
              << std::endl;
  }
};