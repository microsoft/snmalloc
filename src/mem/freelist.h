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

#include <cstdint>

namespace snmalloc
{
#ifdef CHECK_CLIENT
  static constexpr std::size_t PRESERVE_BOTTOM_BITS = 16;

  /**
   * The key that is used to encode free list pointers.
   * This should be randomised at startup in the future.
   */
  inline static address_t global_key = static_cast<std::size_t>(
    bits::is64() ? 0x9999'9999'9999'9999 : 0x9999'9999);
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

  static inline bool different_slab(uintptr_t p1, uintptr_t p2)
  {
    return ((p1 ^ p2) >= SLAB_SIZE);
  }

  static inline bool different_slab(uintptr_t p1, void* p2)
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

  public:
    static inline FreeObject*
    encode(uint16_t local_key, FreeObject* next_object)
    {
#ifdef CHECK_CLIENT
      if constexpr (aal_supports<IntegerPointers>)
      {
        // Simple involutional encoding.  The bottom half of each word is
        // multiplied by a function of both global and local keys (the latter,
        // in practice, being the address of the previous list entry) and the
        // resulting word's top half is XORed into the pointer value before it
        // is stored.
        auto next = address_cast(next_object);
        constexpr uintptr_t MASK = bits::one_at_bit(PRESERVE_BOTTOM_BITS) - 1;
        // Mix in local_key
        // We shift local key to the critical bits have more effect on the high
        // bits.
        address_t lk = local_key;
        auto key = (lk << PRESERVE_BOTTOM_BITS) ^ global_key;
        next ^= (((next & MASK) + 1) * key) & ~MASK;
        next_object = reinterpret_cast<FreeObject*>(next);
      }
#else
      UNUSED(local_key);
#endif
      return next_object;
    }

    void store(FreeObject* value, uint16_t local_key)
    {
      reference = encode(local_key, value);
    }

    FreeObject* read(uint16_t local_key)
    {
      return encode(local_key, reference);
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
    FreeObject* read_next(uint16_t key)
    {
      return next_object.read(key);
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
    uintptr_t prev = 0;
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

    FreeListIter()
    : curr(nullptr)
#ifdef CHECK_CLIENT
      ,
      prev(0)
#endif
    {}

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
    void* take()
    {
#ifdef CHECK_CLIENT
      check_client(
        !different_slab(prev, curr), "Heap corruption - free list corrupted!");
#endif
      auto c = curr;
      update_cursor(curr->read_next(get_prev()));
      return c;
    }
  };

  /**
   * Used to build a free list in object space.
   *
   * Adds signing of pointers
   */
  class FreeListBuilder
  {
    EncodeFreeObjectReference head;
    EncodeFreeObjectReference* end;
#ifdef CHECK_CLIENT
    uint16_t prev;
    uint16_t curr;
#endif

    uint16_t get_prev()
    {
#ifdef CHECK_CLIENT
      return prev;
#else
      return 0;
#endif
    }

    static constexpr uint16_t HEAD_KEY = 1;

  public:
    /**
     * Start building a new free list.
     * Provide pointer to the slab to initialise the system.
     */
    void open(void* p)
    {
      SNMALLOC_ASSERT(empty());
#ifdef CHECK_CLIENT
      prev = HEAD_KEY;
      curr = initial_key(p) & 0xffff;
#else
      UNUSED(p);
#endif
      end = &head;
    }

    /**
     * Returns current head without affecting the builder.
     */
    void* peek_head()
    {
      return head.read(HEAD_KEY);
    }

    /**
     * Checks if there are any more values to iterate.
     */
    bool empty()
    {
      return end == &head;
    }

    /**
     * Adds an element to the free list
     */
    void add(void* n)
    {
      SNMALLOC_ASSERT(!different_slab(end, n) || empty());
      FreeObject* next = FreeObject::make(n);
      end->store(next, get_prev());
      end = &(next->next_object);
#ifdef CHECK_CLIENT
      prev = curr;
      curr = address_cast(next) & 0xffff;
#endif
    }

    /**
     * Adds a terminator at the end of a free list,
     * but does not close the builder.  Thus new elements
     * can still be added.  It returns a new iterator to the
     * list.
     *
     * This is used to iterate an list that is being constructed.
     * It is currently only used to check invariants in Debug builds.
     */
    FreeListIter terminate()
    {
      if (!empty())
        end->store(nullptr, get_prev());
      // Build prev
      auto h = head.read(HEAD_KEY);
      return {h};
    }

    /**
     * Close a free list, and set the iterator parameter
     * to iterate it.
     */
    void close(FreeListIter& dst)
    {
      dst = terminate();
      init();
    }

    /**
     * Set the builder to a not building state.
     */
    void init()
    {
      end = &head;
    }
  };
} // namespace snmalloc
