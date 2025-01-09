#pragma once

#include <array>

namespace snmalloc
{
  namespace proxy
  {
    template<typename T, size_t N>
    using Array = std::array<T, N>;

    using std::begin;
    using std::end;
  } // namespace proxy
} // namespace snmalloc
