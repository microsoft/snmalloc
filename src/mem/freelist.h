#pragma once
/**
 * This file encapsulates the in disused object free lists
 * that are used per slab of small objects.
 */

#include "../ds/address.h"
#include "../ds/cdllist.h"
#include "../ds/dllist.h"
#include "../ds/helpers.h"
#include "allocconfig.h"
#include "entropy.h"

#include <cstdint>

namespace snmalloc
{
#ifdef CHECK_CLIENT
  static constexpr std::size_t PRESERVE_BOTTOM_BITS = 16;
#endif

  /**
   * Used to turn a location into a key.  This is currently
   * just the slab address truncated to 16bits and offset by 1.
   */
  inline static address_t initial_key(void* slab)
  {
#ifdef CHECK_CLIENT
    /**
     * This file assumes that SLAB_BITS is smaller than 16.  In multiple
     * places it uses uint16_t to represent the offset into a slab.
     */
    static_assert(
      SLAB_BITS <= 16,
      "Encoding requires slab offset representable in 16bits.");

    return (address_cast(slab) & SLAB_MASK) + 1;
#else
    UNUSED(slab);
    return 0;
#endif
  }

  static inline bool different_slab(address_t p1, address_t p2)
  {
    return ((p1 ^ p2) >= SLAB_SIZE);
  }

  static inline bool different_slab(address_t p1, void* p2)
  {
    return different_slab(p1, address_cast(p2));
  }

  static inline bool different_slab(void* p1, void* p2)
  {
    return different_slab(address_cast(p1), address_cast(p2));
  }

  class FreeObject;

  class EncodeFreeObjectReference
  {
    FreeObject* reference;

    /**
     * On architectures which use IntegerPointers, we can obfuscate our free
     * lists and use this to drive some probabilistic checks for integrity.
     *
     * There are two definitions of encode() below, which use std::enable_if_t
     * to gate on do_encode.
     */
#ifndef CHECK_CLIENT
    static constexpr bool do_encode = false;
#else
    static constexpr bool do_encode = aal_supports<IntegerPointers, Aal>;
#endif

  public:
#ifdef CHECK_CLIENT
    template<typename T = FreeObject>
    static std::enable_if_t<do_encode, T*>
    encode(uint16_t local_key, T* next_object, LocalEntropy& entropy)
    {
      // Simple involutional encoding.  The bottom half of each word is
      // multiplied by a function of both global and local keys (the latter,
      // in practice, being the address of the previous list entry) and the
      // resulting word's top part is XORed into the pointer value before it
      // is stored.
      auto next = address_cast(next_object);
      constexpr address_t MASK = bits::one_at_bit(PRESERVE_BOTTOM_BITS) - 1;
      // Mix in local_key
      address_t key = (local_key + 1) * entropy.get_constant_key();
      next ^= (((next & MASK) + 1) * key) &
        ~(bits::one_at_bit(PRESERVE_BOTTOM_BITS) - 1);
      return reinterpret_cast<FreeObject*>(next);
    }
#endif

    template<typename T = FreeObject>
    static std::enable_if_t<!do_encode, T*>
    encode(uint16_t local_key, T* next_object, LocalEntropy& entropy)
    {
      UNUSED(local_key);
      UNUSED(entropy);
      return next_object;
    }

    void store(FreeObject* value, uint16_t local_key, LocalEntropy& entropy)
    {
      reference = encode(local_key, value, entropy);
    }

    FreeObject* read(uint16_t local_key, LocalEntropy& entropy)
    {
      return encode(local_key, reference, entropy);
    }
  };

  /**
   * Free objects within each slab point directly to the next.
   * The next_object pointer can be encoded to detect
   * corruption caused by writes in a UAF or a double free.
   *
   * If cache-friendly offsets are used, then the FreeObject is
   * potentially offset from the start of the object.
   */
  class FreeObject
  {
  public:
    EncodeFreeObjectReference next_object;

    static FreeObject* make(void* p)
    {
      return static_cast<FreeObject*>(p);
    }

    /**
     * Read the next pointer handling any required decoding of the pointer
     */
    FreeObject* read_next(uint16_t key, LocalEntropy& entropy)
    {
      return next_object.read(key, entropy);
    }
  };

  /**
   * Used to iterate a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListIter
  {
    FreeObject* curr = nullptr;
#ifdef CHECK_CLIENT
    address_t prev = 0;
#endif

    uint16_t get_prev()
    {
#ifdef CHECK_CLIENT
      return prev & 0xffff;
#else
      return 0;
#endif
    }

    /**
     * Updates the cursor to the new value,
     * importantly this updates the key being used.
     * Currently this is just the value of current before this call.
     * Other schemes could be used.
     */
    void update_cursor(FreeObject* next)
    {
#ifdef CHECK_CLIENT
#  ifndef NDEBUG
      if (next != nullptr)
      {
        check_client(
          !different_slab(prev, next),
          "Heap corruption - free list corrupted!");
      }
#  endif
      prev = address_cast(curr);
#endif
      curr = next;
    }

  public:
    FreeListIter(FreeObject* head)
    : curr(head)
#ifdef CHECK_CLIENT
      ,
      prev(initial_key(head))
#endif
    {
      SNMALLOC_ASSERT(head != nullptr);
    }

    FreeListIter() = default;

    /**
     * Checks if there are any more values to iterate.
     */
    bool empty()
    {
      return curr == nullptr;
    }

    /**
     * Returns current head without affecting the iterator.
     */
    void* peek()
    {
      return curr;
    }

    /**
     * Moves the iterator on, and returns the current value.
     */
    void* take(LocalEntropy& entropy)
    {
#ifdef CHECK_CLIENT
      check_client(
        !different_slab(prev, curr), "Heap corruption - free list corrupted!");
#endif
      auto c = curr;
      update_cursor(curr->read_next(get_prev(), entropy));
      return c;
    }
  };

  /**
   * Used to build a free list in object space.
   *
   * Adds signing of pointers
   *
   * On 64bit ptr architectures this data structure has
   *   44 bytes of data
   * and has an alignment of
   *   8 bytes
   * This unfortunately means its sizeof is 48bytes. We
   * use the template parameter, so that an enclosing
   * class can make use of the remaining four bytes.
   *
   * The builder uses two queues, and "randomly" decides to
   * add to one of the two queues.  This means that we will
   * maintain a randomisation of the order between
   * allocations.
   *
   * The fields are paired up to give better codegen as then they are offset
   * by a power of 2, and the bit extract from the interleaving seed can
   * be shifted to calculate the relevant offset to index the fields.
   */
  template<typename S = uint32_t>
  class FreeListBuilder
  {
    // Pointer to the first element.
    EncodeFreeObjectReference head[2];
    // Pointer to the reference to the last element.
    // In the empty case end[i] == &head[i]
    // This enables branch free enqueuing.
    EncodeFreeObjectReference* end[2];
#ifdef CHECK_CLIENT
    // The bottom 16 bits of the previous pointer
    uint16_t prev[2];
    // The bottom 16 bits of the current pointer
    // This needs to be stored for the empty case
    // where it is `initial_key()` for the slab.
    uint16_t curr[2];
#endif
  public:
    S s;

    uint16_t get_prev(uint32_t index)
    {
#ifdef CHECK_CLIENT
      return prev[index];
#else
      UNUSED(index);
      return 0;
#endif
    }

    uint16_t get_curr(uint32_t index)
    {
#ifdef CHECK_CLIENT
      return curr[index];
#else
      UNUSED(index);
      return 0;
#endif
    }

    static constexpr uint16_t HEAD_KEY = 1;

  public:
    FreeListBuilder()
    {
      init();
    }

    /**
     * Start building a new free list.
     * Provide pointer to the slab to initialise the system.
     */
    void open(void* p)
    {
      SNMALLOC_ASSERT(empty());
#ifdef CHECK_CLIENT
      prev[0] = HEAD_KEY;
      curr[0] = initial_key(p) & 0xffff;
      prev[1] = HEAD_KEY;
      curr[1] = initial_key(p) & 0xffff;
#else
      UNUSED(p);
#endif
      end[0] = &head[0];
      end[1] = &head[1];
    }

    /**
     * Checks if the builder contains any elements.
     */
    bool empty()
    {
      return end[0] == &head[0] && end[1] == &head[1];
    }

    /**
     * Adds an element to the builder
     */
    void add(void* n, LocalEntropy& entropy)
    {
      SNMALLOC_ASSERT(
        !different_slab(end[0], n) || !different_slab(end[1], n) || empty());
      FreeObject* next = FreeObject::make(n);

      auto index = entropy.next_bit();

      end[index]->store(next, get_prev(index), entropy);
      end[index] = &(next->next_object);
#ifdef CHECK_CLIENT
      prev[index] = curr[index];
      curr[index] = address_cast(next) & 0xffff;
#endif
    }

    /**
     *  Calculates the length of the queue.
     *  This is O(n) as it walks the queue.
     *  If this is needed in a non-debug setting then
     *  we should look at redesigning the queue.
     */
    size_t debug_length(LocalEntropy& entropy)
    {
      size_t count = 0;
      for (size_t i = 0; i < 2; i++)
      {
        uint16_t local_prev = HEAD_KEY;
        EncodeFreeObjectReference* iter = &head[i];
        FreeObject* prev_obj = iter->read(local_prev, entropy);
        uint16_t local_curr = initial_key(prev_obj) & 0xffff;
        while (end[i] != iter)
        {
          FreeObject* next = iter->read(local_prev, entropy);
          check_client(!different_slab(next, prev_obj), "Heap corruption");
          local_prev = local_curr;
          local_curr = address_cast(next) & 0xffff;
          count++;
          iter = &next->next_object;
        }
      }
      return count;
    }

    /**
     * Adds a terminator at the end of a free list,
     * but does not close the builder.  Thus new elements
     * can still be added.  It returns a new iterator to the
     * list.
     *
     * This also collapses the two queues into one, so that it can
     * be iterated easily.
     *
     * This is used to iterate an list that is being constructed.
     *
     * It is used with preserve_queue enabled to check
     * invariants in Debug builds.
     *
     * It is used with preserve_queue disabled by close.
     */
    FreeListIter terminate(LocalEntropy& entropy, bool preserve_queue = true)
    {
      SNMALLOC_ASSERT(end[1] != &head[0]);
      SNMALLOC_ASSERT(end[0] != &head[1]);

      // If second list is empty, then append is trivial.
      if (end[1] == &head[1])
      {
        end[0]->store(nullptr, get_prev(0), entropy);
        return {head[0].read(HEAD_KEY, entropy)};
      }

      end[1]->store(nullptr, get_prev(1), entropy);

      // Append 1 to 0
      auto mid = head[1].read(HEAD_KEY, entropy);
      end[0]->store(mid, get_prev(0), entropy);
      // Re-code first link in second list (if there is one).
      // The first link in the second list will be encoded with initial_key,
      // But that needs to be changed to the curr of the first list.
      if (mid != nullptr)
      {
        auto mid_next = mid->read_next(initial_key(mid) & 0xffff, entropy);
        mid->next_object.store(mid_next, get_curr(0), entropy);
      }

      auto h = head[0].read(HEAD_KEY, entropy);

      // If we need to continue adding to the builder
      // Set up the second list as empty,
      // and extend the first list to cover all of the second.
      if (preserve_queue && h != nullptr)
      {
#ifdef CHECK_CLIENT
        prev[0] = prev[1];
        curr[0] = curr[1];
#endif
        end[0] = end[1];
#ifdef CHECK_CLIENT
        prev[1] = HEAD_KEY;
        curr[1] = initial_key(h) & 0xffff;
#endif
        end[1] = &(head[1]);
      }

      SNMALLOC_ASSERT(end[1] != &head[0]);
      SNMALLOC_ASSERT(end[0] != &head[1]);

      return {h};
    }

    /**
     * Close a free list, and set the iterator parameter
     * to iterate it.
     */
    void close(FreeListIter& dst, LocalEntropy& entropy)
    {
      dst = terminate(entropy, false);
      init();
    }

    /**
     * Set the builder to a not building state.
     */
    void init()
    {
      end[0] = &head[0];
      end[1] = &head[1];
    }
  };
} // namespace snmalloc
