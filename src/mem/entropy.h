#include "../ds/address.h"
#include "../pal/pal.h"

#include <cstdint>
#include <random>
#include <type_traits>

namespace snmalloc
{
  template<typename PAL>
  std::enable_if_t<pal_supports<Entropy, PAL>, uint64_t> get_entropy64()
  {
    return PAL::get_entropy64();
  }

  template<typename PAL>
  std::enable_if_t<!pal_supports<Entropy, PAL>, uint64_t> get_entropy64()
  {
    std::random_device rd;
    uint64_t a = rd();
    return (a << 32) ^ rd();
  }

  class LocalEntropy
  {
    uint64_t bit_source;
    uint64_t local_key;
    uint64_t local_counter;
    address_t constant_key;

  public:
    template<typename PAL>
    void init()
    {
      local_key = get_entropy64<PAL>();
      local_counter = get_entropy64<PAL>();
      if constexpr (bits::BITS == 64)
        constant_key = get_next();
      else
        constant_key = get_next() & 0xffff'ffff;
      bit_source = get_next();
    }

    /**
     * Returns a bit.
     *
     * The bit returned is cycled every 64 calls.
     * This is a very cheap source of some randomness.
     * Returns the bottom bit.
     */
    uint32_t next_bit()
    {
      uint64_t bottom_bit = bit_source & 1;
      bit_source = (bottom_bit << 63) | (bit_source >> 1);
      return bottom_bit & 1;
    }

    /**
     * A key that is not changed or used to create other keys
     *
     * This is for use when there is no storage for the key.
     */
    address_t get_constant_key()
    {
      return constant_key;
    }

    /**
     * Source of random 64bit values
     *
     * Has a 2^64 period.
     *
     * Applies a Feistel cipher to a counter
     */
    uint64_t get_next()
    {
      uint64_t c = ++local_counter;
      uint64_t bottom;
      for (int i = 0; i < 2; i++)
      {
        bottom = c & 0xffff'fffff;
        c = (c << 32) | (((bottom * local_key) ^ c) >> 32);
      }
      return c;
    }

    /**
     * Refresh `next_bit` source of bits.
     *
     * This loads new entropy into the `next_bit` values.
     */
    void refresh_bits()
    {
      bit_source = get_next();
    }
  };
} // namespace snmalloc