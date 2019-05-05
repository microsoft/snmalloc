#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>

#define DO_TIME(name, code) \
  { \
    auto start__ = std::chrono::high_resolution_clock::now(); \
    code auto finish__ = std::chrono::high_resolution_clock::now(); \
    auto diff__ = finish__ - start__; \
    std::cout << name << ": " << std::setw(12) << diff__.count() << " ns" \
              << std::endl; \
  }
