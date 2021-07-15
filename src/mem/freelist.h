#pragma once
/**
 * This file encapsulates the in disused object free lists
 * that are used per slab of small objects. The implementation
 * can be configured to introduce randomness to the reallocation,
 * and also provide signing to detect free list corruption.
 *
 * # Corruption
 *
 * The corruption detection works as follows
 *
 *   FreeObject
 *   -----------------------------
 *   | next | prev_encoded | ... |
 *   -----------------------------
 * A free object contains a pointer to next object in the free list, and
 * a prev pointer, but the prev pointer is really a signature with the
 * following property
 *
 *  If n = c->next && n != 0, then n->prev_encoded = f(c,n).
 *
 * If f just returns the first parameter, then this degenerates to a doubly
 * linked list.  (Note that doing the degenerate case can be useful for
 * debugging snmalloc bugs.) By making it a function of both pointers, it
 * makes it harder for an adversary to mutate prev_encoded to a valid value.
 *
 * This provides protection against the free-list being corrupted by memory
 * safety issues.
 *
 * # Randomness
 *
 * The randomness is introduced by building two free lists simulatenously,
 * and randomly deciding which list to add an element to.
 */

#include "../ds/address.h"
#include "allocconfig.h"
#include "entropy.h"

#include <cstdint>

namespace snmalloc
{
  struct Remote;

  /**
   * This function is used to sign back pointers in the free list.
   *
   * TODO - Needs review.  Should we shift low bits out as they help
   * guess the key.
   *
   * TODO - We now have space in the FreeListBuilder for a fresh key for each
   * list.
   */
  inline static uintptr_t
  signed_prev(address_t curr, address_t next, LocalEntropy& entropy)
  {
    auto c = curr;
    auto n = next;
    auto k = entropy.get_constant_key();
    return (c + k) * (n - k);
  }

  /**
   * Free objects within each slab point directly to the next.
   * There is an optional second field that is effectively a
   * back pointer in a doubly linked list, however, it is encoded
   * to prevent corruption.
   */
  class FreeObject
  {
    CapPtr<FreeObject, CBAlloc> next_object;
#ifdef CHECK_CLIENT
    // Encoded representation of a back pointer.
    // Hard to fake, and provides consistency on
    // the next pointers.
    address_t prev_encoded;
#endif

  public:
    static CapPtr<FreeObject, CBAlloc> make(CapPtr<void, CBAlloc> p)
    {
      return p.template as_static<FreeObject>();
    }

    /**
     * Construct a FreeObject for local slabs from a Remote message.
     */
    static CapPtr<FreeObject, CBAlloc> make(CapPtr<Remote, CBAlloc> p)
    {
      // TODO: Zero the difference between a FreeObject and a Remote
      return p.template as_reinterpret<FreeObject>();
    }

    /**
     * Assign next_object and update its prev_encoded if CHECK_CLIENT.
     *
     * Static so that it can be used on reference to a FreeObject.
     *
     * Returns a pointer to the next_object field of the next parameter as an
     * optimization for repeated snoc operations (in which
     * next->next_object is nullptr).
     */
    static CapPtr<FreeObject, CBAlloc>* store(
      CapPtr<FreeObject, CBAlloc>* curr,
      CapPtr<FreeObject, CBAlloc> next,
      LocalEntropy& entropy)
    {
      *curr = next;
#ifdef CHECK_CLIENT
      next->prev_encoded =
        signed_prev(address_cast(curr), address_cast(next), entropy);
#else
      UNUSED(entropy);
#endif
      return &(next->next_object);
    }

    /**
     * Check the signature of this FreeObject
     */
    void check_prev(address_t signed_prev)
    {
      UNUSED(signed_prev);
      check_client(
        signed_prev == prev_encoded, "Heap corruption - free list corrupted!");
    }

    /**
     * Read the next pointer
     */
    CapPtr<FreeObject, CBAlloc> read_next()
    {
      return next_object;
    }
  };

  /**
   * Used to iterate a free list in object space.
   *
   * Checks signing of pointers
   */
  class FreeListIter
  {
    CapPtr<FreeObject, CBAlloc> curr{nullptr};
#ifdef CHECK_CLIENT
    address_t prev{0};
#endif

  public:
    constexpr FreeListIter(
      CapPtr<FreeObject, CBAlloc> head, address_t prev_value)
    : curr(head)
    {
#ifdef CHECK_CLIENT
      prev = prev_value;
#endif
      UNUSED(prev_value);
    }

    constexpr FreeListIter() = default;

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
    CapPtr<FreeObject, CBAlloc> peek()
    {
      return curr;
    }

    /**
     * Moves the iterator on, and returns the current value.
     */
    CapPtr<FreeObject, CBAlloc> take(LocalEntropy& entropy)
    {
      auto c = curr;
      auto next = curr->read_next();

      Aal::prefetch(next.unsafe_ptr());
      curr = next;
#ifdef CHECK_CLIENT
      c->check_prev(prev);
      prev = signed_prev(address_cast(c), address_cast(next), entropy);
#else
      UNUSED(entropy);
#endif

      return c;
    }
  };

  /**
   * Used to build a free list in object space.
   *
   * Adds signing of pointers in the CHECK_CLIENT mode
   *
   * We use the template parameter, so that an enclosing
   * class can make use of the remaining bytes, which may not
   * be aligned.  On 64bit ptr architectures, this structure
   * is a multiple of 8 bytes in the checked and random more.
   * But on 128bit ptr architectures this may be a benefit.
   *
   * If RANDOM is enabled, the builder uses two queues, and
   * "randomly" decides to add to one of the two queues.  This
   * means that we will maintain a randomisation of the order
   * between allocations.
   *
   * The fields are paired up to give better codegen as then they are offset
   * by a power of 2, and the bit extract from the interleaving seed can
   * be shifted to calculate the relevant offset to index the fields.
   *
   * If RANDOM is set to false, then the code does not perform any
   * randomisation.
   */
  template<bool RANDOM, typename S = uint32_t>
  class FreeListBuilder
  {
    static constexpr size_t LENGTH = RANDOM ? 2 : 1;

    // Pointer to the first element.
    CapPtr<FreeObject, CBAlloc> head[LENGTH];
    // Pointer to the reference to the last element.
    // In the empty case end[i] == &head[i]
    // This enables branch free enqueuing.
    CapPtr<FreeObject, CBAlloc>* end[LENGTH];

  public:
    S s;

  public:
    constexpr FreeListBuilder()
    {
      init();
    }

    /**
     * Checks if the builder contains any elements.
     */
    bool empty()
    {
      for (size_t i = 0; i < LENGTH; i++)
      {
        if (address_cast(end[i]) != address_cast(&head[i]))
          return false;
      }
      return true;
    }

    /**
     * Adds an element to the builder
     */
    void add(CapPtr<FreeObject, CBAlloc> n, LocalEntropy& entropy)
    {
      uint32_t index;
      if constexpr (RANDOM)
        index = entropy.next_bit();
      else
        index = 0;

      end[index] = FreeObject::store(end[index], n, entropy);
    }

    /**
     * Makes a terminator to a free list.
     *
     * Termination uses the bottom bit, this allows the next pointer
     * to always be to the same slab.
     */
    SNMALLOC_FAST_PATH void
    terminate_list(uint32_t index, LocalEntropy& entropy)
    {
      UNUSED(entropy);
      *end[index] = nullptr;
    }

    address_t get_fake_signed_prev(uint32_t index, LocalEntropy& entropy)
    {
      return signed_prev(
        address_cast(&head[index]), address_cast(head[index]), entropy);
    }

    /**
     * Close a free list, and set the iterator parameter
     * to iterate it.
     */
    SNMALLOC_FAST_PATH void close(FreeListIter& fl, LocalEntropy& entropy)
    {
      if constexpr (RANDOM)
      {
        SNMALLOC_ASSERT(end[1] != &head[0]);
        SNMALLOC_ASSERT(end[0] != &head[1]);

        // If second list is non-empty, perform append.
        if (end[1] != &head[1])
        {
          // The start token has been corrupted.
          // TOCTTOU issue, but small window here.
          head[1]->check_prev(get_fake_signed_prev(1, entropy));

          terminate_list(1, entropy);

          // Append 1 to 0
          FreeObject::store(end[0], head[1], entropy);

          SNMALLOC_ASSERT(end[1] != &head[0]);
          SNMALLOC_ASSERT(end[0] != &head[1]);
        }
        else
        {
          terminate_list(0, entropy);
        }
      }
      else
      {
        terminate_list(0, entropy);
      }

      fl = {head[0], get_fake_signed_prev(0, entropy)};
      init();
    }

    /**
     * Set the builder to a not building state.
     */
    constexpr void init()
    {
      for (size_t i = 0; i < LENGTH; i++)
      {
        end[i] = &head[i];
      }
    }
  };
} // namespace snmalloc
