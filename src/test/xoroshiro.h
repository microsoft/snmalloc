#pragma once

#include <cstdint>
#include <cstdlib>

namespace xoroshiro
{
  namespace detail
  {
    template<typename STATE, typename RESULT, STATE A, STATE B, STATE C>
    class XorOshiro
    {
    private:
      static constexpr unsigned STATE_BITS = 8 * sizeof(STATE);
      static constexpr unsigned RESULT_BITS = 8 * sizeof(RESULT);

      static_assert(
        STATE_BITS >= RESULT_BITS,
        "STATE must have at least as many bits as RESULT");

      STATE x;
      STATE y;

      static inline STATE rotl(STATE x, STATE k)
      {
        return (x << k) | (x >> (STATE_BITS - k));
      }

    public:
      XorOshiro(STATE x_ = 5489, STATE y_ = 0) : x(x_), y(y_)
      {
        // If both zero, then this does not work
        if (x_ == 0 && y_ == 0)
          abort();

        next();
      }

      void set_state(STATE x_, STATE y_ = 0)
      {
        // If both zero, then this does not work
        if (x_ == 0 && y_ == 0)
          abort();

        x = x_;
        y = y_;
        next();
      }

      RESULT next()
      {
        STATE r = x + y;
        y ^= x;
        x = rotl(x, A) ^ y ^ (y << B);
        y = rotl(y, C);
        // If both zero, then this does not work
        if (x == 0 && y == 0)
          abort();
        return r >> (STATE_BITS - RESULT_BITS);
      }
    };
  }

  using p128r64 = detail::XorOshiro<uint64_t, uint64_t, 55, 14, 36>;
  using p128r32 = detail::XorOshiro<uint64_t, uint32_t, 55, 14, 36>;
  using p64r32 = detail::XorOshiro<uint32_t, uint32_t, 27, 7, 20>;
  using p64r16 = detail::XorOshiro<uint32_t, uint16_t, 27, 7, 20>;
  using p32r16 = detail::XorOshiro<uint16_t, uint16_t, 13, 5, 10>;
  using p32r8 = detail::XorOshiro<uint16_t, uint8_t, 13, 5, 10>;
  using p16r8 = detail::XorOshiro<uint8_t, uint8_t, 4, 7, 3>;
}
