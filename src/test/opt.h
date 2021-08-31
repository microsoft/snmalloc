#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace opt
{
  class Opt
  {
  private:
    int argc;
    const char* const* argv;

  public:
    Opt(int argc, const char* const* argv) : argc(argc), argv(argv) {}

    bool has(const char* opt)
    {
      for (int i = 1; i < argc; i++)
      {
        if (!strcmp(opt, argv[i]))
          return true;
      }

      return false;
    }

    template<class T>
    T is(const char* opt, T def)
    {
      size_t len = strlen(opt);

      for (int i = 1; i < argc; i++)
      {
        const char* p = param(opt, len, i);

        if (p != nullptr)
        {
          char* end = nullptr;
          T r;

          if (std::is_unsigned<T>::value)
            r = (T)strtoull(p, &end, 10);
          else
            r = (T)strtoll(p, &end, 10);

          if ((r == 0) && (end == p))
            return def;

          return r;
        }
      }

      return def;
    }

    const char* is(const char* opt, const char* def)
    {
      size_t len = strlen(opt);

      for (int i = 1; i < argc; i++)
      {
        const char* p = param(opt, len, i);

        if (p != nullptr)
          return p;
      }

      return def;
    }

  private:
    const char* param(const char* opt, size_t len, int i)
    {
      if (strncmp(opt, argv[i], len))
        return nullptr;

      switch (argv[i][len])
      {
        case '\0':
          return (i < (argc - 1)) ? argv[i + 1] : nullptr;
        case '=':
          return &argv[i][len + 1];
        default:
          return nullptr;
      }
    }
  };
}
