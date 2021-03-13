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

#include <iostream>

namespace snmalloc
{
#ifdef CHECK_CLIENT
  /**
   * The key that is used to encode free list pointers.
   * This should be randomised at startup in the future.
   */
  inline static address_t global_key =
    static_cast<size_t>(bits::is64() ? 0x9999'9999'9999'9999 : 0x9999'9999);

  /**
   * Used to turn a location into a key.  This is currently
   * just the value of the previous location + 1.
   */
  inline static uintptr_t initial_key(void* p)
  {
    return address_cast(p) + 1;
  }
#endif

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
    FreeObject* next_object;

    static FreeObject* encode(uintptr_t local_key, FreeObject* next_object)
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
        constexpr uintptr_t MASK = bits::one_at_bit(bits::BITS / 2) - 1;
        // Mix in local_key
        auto key = local_key ^ global_key;
        next ^= (((next & MASK) + 1) * key) & ~MASK;
        next_object = reinterpret_cast<FreeObject*>(next);
      }
#else
      UNUSED(local_key);
#endif
      return next_object;
    }

  public:
    static FreeObject* make(void* p)
    {
      return static_cast<FreeObject*>(p);
    }

    /**
     * Read the next pointer handling any required decoding of the pointer
     */
    FreeObject* read_next(uintptr_t key)
    {
      auto next = encode(key, next_object);
      return next;
    }

    /**
     * Store the next pointer handling any required encoding of the pointer
     */
    void store_next(FreeObject* next, uintptr_t key)
    {
      next_object = encode(key, next);
      SNMALLOC_ASSERT(next == read_next(key));
    }
  };

  /**
   * Wrapper class that allows the keys for pointer encoding to be
   * conditionally compiled.
   */
  class FreeObjectCursor
  {
    FreeObject* curr = nullptr;
#ifdef CHECK_CLIENT
    uintptr_t prev = 0;
#endif

    uintptr_t get_prev()
    {
#ifdef CHECK_CLIENT
      return prev;
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
        if (unlikely(different_slab(prev, next)))
        {
          error("Heap corruption - free list corrupted!");
        }
      }
#  endif
      prev = address_cast(curr);
#endif
      curr = next;
    }

  public:
    FreeObject* get_curr()
    {
      return curr;
    }

    /**
     * Advance the cursor through the list
     */
    void move_next()
    {
#ifdef CHECK_CLIENT
      if (unlikely(different_slab(prev, curr)))
      {
        error("Heap corruption - free list corrupted!");
      }
#endif
      update_cursor(curr->read_next(get_prev()));
    }

    /**
     * Update the next pointer at the location in the list pointed to
     * by the cursor.
     */
    void set_next(FreeObject* next)
    {
      curr->store_next(next, get_prev());
    }

    /**
     * Update the next pointer at the location in the list pointed to
     * by the cursor, and move the cursor to that new value.
     */
    void set_next_and_move(FreeObject* next)
    {
      set_next(next);
      update_cursor(next);
    }

    /**
     * Resets the key to an initial value. So the cursor can be used
     * on a new sequence.
     */
    void reset_cursor(FreeObject* next)
    {
#ifdef CHECK_CLIENT
      prev = initial_key(next);
#endif
      curr = next;
    }
  };

  /**
   * Used to iterate a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListIter
  {
  protected:
    FreeObjectCursor front;

  public:
    /**
     * Checks if there are any more values to iterate.
     */
    bool empty()
    {
      return front.get_curr() == nullptr;
    }

    /**
     * Moves the iterator on, and returns the current value.
     */
    void* take()
    {
      auto c = front.get_curr();
      front.move_next();
      return c;
    }
  };

  /**
   * Used to build a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListBuilder : FreeListIter
  {
    FreeObjectCursor end;

  public:
    /**
     * Start building a new free list.
     */
    void open(void* n)
    {
      SNMALLOC_ASSERT(empty());
      FreeObject* next = FreeObject::make(n);
      end.reset_cursor(next);
      front.reset_cursor(next);
    }

    /**
     * Returns current head without affecting the builder.
     */
    void* peek_head()
    {
      return front.get_curr();
    }

    /**
     * Checks if there are any more values to iterate.
     */
    bool empty()
    {
      return FreeListIter::empty();
    }

    /**
     * Adds an element to the free list
     */
    void add(void* n)
    {
      SNMALLOC_ASSERT(!different_slab(end.get_curr(), n));
      FreeObject* next = FreeObject::make(n);
      end.set_next_and_move(next);
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
        end.set_next(nullptr);
      return *this;
    }

    /**
     * Close a free list, and set the iterator parameter
     * to iterate it.
     */
    void close(FreeListIter& dst)
    {
      terminate();
      dst = *this;
      init();
    }

    /**
     * Set the builder to a not building state.
     */
    void init()
    {
      front.reset_cursor(nullptr);
    }
  };
} // namespace snmalloc
