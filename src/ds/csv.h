#pragma once

#include <iostream>
#include <string>

namespace snmalloc
{
  class CSVStream
  {
  private:
    std::ostream* out;
    bool first = true;

  public:
    class Endl
    {};

    Endl endl;

    CSVStream(std::ostream* o) : out(o) {}

    void preprint()
    {
      if (!first)
      {
        *out << ", ";
      }
      else
      {
        first = false;
      }
    }

    CSVStream& operator<<(const std::string& str)
    {
      preprint();
      *out << str;
      return *this;
    }

    CSVStream& operator<<(uint64_t u)
    {
      preprint();
      *out << u;
      return *this;
    }

    CSVStream& operator<<(Endl)
    {
      *out << std::endl;
      first = true;
      return *this;
    }
  };
} // namespace snmalloc