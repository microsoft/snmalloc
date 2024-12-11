#pragma once

namespace snmalloc
{
  class StringView
  {
    size_t len;
    const char* str;

  public:
    static constexpr size_t length(const char* s)
    {
      size_t len = 0;
      while (*s != 0)
      {
        len++;
        s++;
      }
      return len;
    }

    template<size_t N>
    constexpr StringView(const char (&s)[N]) : len(N - 1), str(s)
    {}

    constexpr StringView(const char* s) : len(length(s)), str(s) {}

    [[nodiscard]] const char* begin() const
    {
      return str;
    }

    [[nodiscard]] const char* end() const
    {
      return str + len;
    }

    [[nodiscard]] size_t size() const
    {
      return len;
    }
  };
}