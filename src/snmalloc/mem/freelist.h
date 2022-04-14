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
 *   free Object
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

#include "../ds/ds.h"
#include "entropy.h"

#include <cstdint>

namespace snmalloc
{
  /**
   * This function is used to sign back pointers in the free list.
   */
  inline static address_t
  signed_prev(address_t curr, address_t next, const FreeListKey& key)
  {
    auto c = curr;
    auto n = next;
    return (c + key.key1) * (n + key.key2);
  }

  namespace freelist
  {
    class Object
    {
    public:
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue =
          capptr::bounds::AllocWild>
      class T;

      /**
       * This "inductive step" type -- a queue-annotated pointer to a free
       * Object containing a queue-annotated pointer -- shows up all over the
       * place.  Give it a shorter name (Object::BQueuePtr<BQueue>) for
       * convenience.
       */
      template<SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      using BQueuePtr = CapPtr<Object::T<BQueue>, BQueue>;

      /**
       * As with BQueuePtr, but atomic.
       */
      template<SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      using BAtomicQueuePtr = AtomicCapPtr<Object::T<BQueue>, BQueue>;

      /**
       * This is the "base case" of that induction.  While we can't get rid of
       * the two different type parameters (in general), we can at least get rid
       * of a bit of the clutter.  "freelist::Object::HeadPtr<BView, BQueue>"
       * looks a little nicer than "CapPtr<freelist::Object::T<BQueue>, BView>".
       */
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      using BHeadPtr = CapPtr<Object::T<BQueue>, BView>;

      /**
       * As with BHeadPtr, but atomic.
       */
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      using BAtomicHeadPtr = AtomicCapPtr<Object::T<BQueue>, BView>;

      /**
       * Free objects within each slab point directly to the next.
       * There is an optional second field that is effectively a
       * back pointer in a doubly linked list, however, it is encoded
       * to prevent corruption.
       *
       * This is an inner class to avoid the need to specify BQueue when calling
       * static methods.
       *
       * Raw C++ pointers to this type are *assumed to be domesticated*.  In
       * some cases we still explicitly annotate domesticated free Object*-s as
       * CapPtr<>, but more often CapPtr<Object::T<A>,B> will have B = A.
       *
       * TODO: Consider putting prev_encoded at the end of the object, would
       * require size to be threaded through, but would provide more OOB
       * detection.
       */
      template<SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      class T
      {
        template<
          bool,
          bool,
          SNMALLOC_CONCEPT(capptr::ConceptBound),
          SNMALLOC_CONCEPT(capptr::ConceptBound)>
        friend class Builder;

        friend class Object;

        union
        {
          BQueuePtr<BQueue> next_object;
          // TODO: Should really use C++20 atomic_ref rather than a union.
          BAtomicQueuePtr<BQueue> atomic_next_object;
        };
#ifdef SNMALLOC_CHECK_CLIENT
        // Encoded representation of a back pointer.
        // Hard to fake, and provides consistency on
        // the next pointers.
        address_t prev_encoded;
#endif

      public:
        template<
          SNMALLOC_CONCEPT(capptr::ConceptBound) BView = typename BQueue::
            template with_wildness<capptr::dimension::Wildness::Tame>,
          typename Domesticator>
        BHeadPtr<BView, BQueue>
        atomic_read_next(const FreeListKey& key, Domesticator domesticate)
        {
          auto n_wild = Object::decode_next(
            address_cast(&this->next_object),
            this->atomic_next_object.load(std::memory_order_acquire),
            key);
          auto n_tame = domesticate(n_wild);
#ifdef SNMALLOC_CHECK_CLIENT
          if (n_tame != nullptr)
          {
            n_tame->check_prev(
              signed_prev(address_cast(this), address_cast(n_tame), key));
          }
#endif
          return n_tame;
        }

        /**
         * Read the next pointer
         */
        template<
          SNMALLOC_CONCEPT(capptr::ConceptBound) BView = typename BQueue::
            template with_wildness<capptr::dimension::Wildness::Tame>,
          typename Domesticator>
        BHeadPtr<BView, BQueue>
        read_next(const FreeListKey& key, Domesticator domesticate)
        {
          return domesticate(Object::decode_next(
            address_cast(&this->next_object), this->next_object, key));
        }

        /**
         * Check the signature of this free Object
         */
        void check_prev(address_t signed_prev)
        {
          UNUSED(signed_prev);
          snmalloc_check_client(
            signed_prev == this->prev_encoded,
            "Heap corruption - free list corrupted!");
        }

        /**
         * Clean up this object when removing it from the list. This is
         * important on CHERI to avoid leaking capabilities. On CHECK_CLIENT
         * builds it might increase the difficulty to bypass the checks.
         */
        void cleanup()
        {
#if defined(__CHERI_PURE_CAPABILITY__) || defined(SNMALLOC_CHECK_CLIENT)
          this->next_object = nullptr;
#  ifdef SNMALLOC_CHECK_CLIENT
          this->prev_encoded = 0;
#  endif
#endif
        }
      };

      // Note the inverted template argument order, since BView is inferable.
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView>
      static BHeadPtr<BView, BQueue> make(CapPtr<void, BView> p)
      {
        return p.template as_static<Object::T<BQueue>>();
      }

      /**
       * A container-of operation to convert &f->next_object to f
       */
      template<SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      static Object::T<BQueue>*
      from_next_ptr(CapPtr<Object::T<BQueue>, BQueue>* ptr)
      {
        static_assert(offsetof(Object::T<BQueue>, next_object) == 0);
        return reinterpret_cast<Object::T<BQueue>*>(ptr);
      }

    private:
      /**
       * Involutive encryption with raw pointers
       */
      template<SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      inline static Object::T<BQueue>*
      code_next(address_t curr, Object::T<BQueue>* next, const FreeListKey& key)
      {
        // Note we can consider other encoding schemes here.
        //   * XORing curr and next.  This doesn't require any key material
        //   * XORing (curr * key). This makes it harder to guess the underlying
        //     key, as each location effectively has its own key.
        // Curr is not used in the current encoding scheme.
        UNUSED(curr);

        if constexpr (CHECK_CLIENT && !aal_supports<StrictProvenance>)
        {
          return unsafe_from_uintptr<Object::T<BQueue>>(
            unsafe_to_uintptr<Object::T<BQueue>>(next) ^ key.key_next);
        }
        else
        {
          UNUSED(key);
          return next;
        }
      }

    public:
      /**
       * Encode next.  We perform two convenient little bits of type-level
       * sleight of hand here:
       *
       *  1) We convert the provided HeadPtr to a QueuePtr, forgetting BView in
       *  the result; all the callers write the result through a pointer to a
       *  QueuePtr, though, strictly, the result itself is no less domesticated
       *  than the input (even if it is obfuscated).
       *
       *  2) Speaking of obfuscation, we continue to use a CapPtr<> type even
       *  though the result is likely not safe to dereference, being an
       *  obfuscated bundle of bits (on non-CHERI architectures, anyway). That's
       *  additional motivation to consider the result BQueue-bounded, as that
       * is likely (but not necessarily) Wild.
       */
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      inline static BQueuePtr<BQueue> encode_next(
        address_t curr, BHeadPtr<BView, BQueue> next, const FreeListKey& key)
      {
        return BQueuePtr<BQueue>(code_next(curr, next.unsafe_ptr(), key));
      }

      /**
       * Decode next.  While traversing a queue, BView and BQueue here will
       * often be equal (i.e., AllocUserWild) rather than dichotomous. However,
       * we do occasionally decode an actual head pointer, so be polymorphic
       * here.
       *
       * TODO: We'd like, in some sense, to more tightly couple or integrate
       * this into to the domestication process.  We could introduce an
       * additional state in the capptr_bounds::wild taxonomy (e.g, Obfuscated)
       * so that the Domesticator-s below have to call through this function to
       * get the Wild pointer they can then make Tame.  It's not yet entirely
       * clear what that would look like and whether/how the encode_next side of
       * things should be exposed.  For the moment, obfuscation is left
       * encapsulated within Object and we do not capture any of it statically.
       */
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      inline static BHeadPtr<BView, BQueue> decode_next(
        address_t curr, BHeadPtr<BView, BQueue> next, const FreeListKey& key)
      {
        return BHeadPtr<BView, BQueue>(code_next(curr, next.unsafe_ptr(), key));
      }

      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      static void assert_view_queue_bounds()
      {
        static_assert(
          BView::wildness == capptr::dimension::Wildness::Tame,
          "Free Object View must be domesticated, justifying raw pointers");

        static_assert(
          std::is_same_v<
            typename BQueue::template with_wildness<
              capptr::dimension::Wildness::Tame>,
            BView>,
          "Free Object Queue bounds must match View bounds (but may be Wild)");
      }

      /**
       * Assign next_object and update its prev_encoded if
       * SNMALLOC_CHECK_CLIENT. Static so that it can be used on reference to a
       * free Object.
       *
       * Returns a pointer to the next_object field of the next parameter as an
       * optimization for repeated snoc operations (in which
       * next->next_object is nullptr).
       */
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      static BQueuePtr<BQueue>* store_next(
        BQueuePtr<BQueue>* curr,
        BHeadPtr<BView, BQueue> next,
        const FreeListKey& key)
      {
        assert_view_queue_bounds<BView, BQueue>();

#ifdef SNMALLOC_CHECK_CLIENT
        next->prev_encoded =
          signed_prev(address_cast(curr), address_cast(next), key);
#else
        UNUSED(key);
#endif
        *curr = encode_next(address_cast(curr), next, key);
        return &(next->next_object);
      }

      template<SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      static void store_null(BQueuePtr<BQueue>* curr, const FreeListKey& key)
      {
        *curr =
          encode_next(address_cast(curr), BQueuePtr<BQueue>(nullptr), key);
      }

      /**
       * Assign next_object and update its prev_encoded if SNMALLOC_CHECK_CLIENT
       *
       * Uses the atomic view of next, so can be used in the message queues.
       */
      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      static void atomic_store_next(
        BHeadPtr<BView, BQueue> curr,
        BHeadPtr<BView, BQueue> next,
        const FreeListKey& key)
      {
        static_assert(BView::wildness == capptr::dimension::Wildness::Tame);

#ifdef SNMALLOC_CHECK_CLIENT
        next->prev_encoded =
          signed_prev(address_cast(curr), address_cast(next), key);
#else
        UNUSED(key);
#endif
        // Signature needs to be visible before item is linked in
        // so requires release semantics.
        curr->atomic_next_object.store(
          encode_next(address_cast(&curr->next_object), next, key),
          std::memory_order_release);
      }

      template<
        SNMALLOC_CONCEPT(capptr::ConceptBound) BView,
        SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue>
      static void
      atomic_store_null(BHeadPtr<BView, BQueue> curr, const FreeListKey& key)
      {
        static_assert(BView::wildness == capptr::dimension::Wildness::Tame);

        curr->atomic_next_object.store(
          encode_next(
            address_cast(&curr->next_object), BQueuePtr<BQueue>(nullptr), key),
          std::memory_order_relaxed);
      }
    };

    static_assert(
      sizeof(Object) <= MIN_ALLOC_SIZE,
      "Needs to be able to fit in smallest allocation.");

    /**
     * External code almost always uses Alloc and AllocWild for its free lists.
     * Give them a convenient alias.
     */
    using HeadPtr =
      Object::BHeadPtr<capptr::bounds::Alloc, capptr::bounds::AllocWild>;

    /**
     * Like HeadPtr, but atomic
     */
    using AtomicHeadPtr =
      Object::BAtomicHeadPtr<capptr::bounds::Alloc, capptr::bounds::AllocWild>;

    /**
     * External code's inductive cases almost always use AllocWild.
     */
    using QueuePtr = Object::BQueuePtr<capptr::bounds::AllocWild>;

    /**
     * Like QueuePtr, but atomic
     */
    using AtomicQueuePtr = Object::BAtomicQueuePtr<capptr::bounds::AllocWild>;

    /**
     * Used to iterate a free list in object space.
     *
     * Checks signing of pointers
     */
    template<
      SNMALLOC_CONCEPT(capptr::ConceptBound) BView = capptr::bounds::Alloc,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue = capptr::bounds::AllocWild>
    class Iter
    {
      Object::BHeadPtr<BView, BQueue> curr{nullptr};
#ifdef SNMALLOC_CHECK_CLIENT
      address_t prev{0};
#endif

    public:
      constexpr Iter(Object::BHeadPtr<BView, BQueue> head, address_t prev_value)
      : curr(head)
      {
#ifdef SNMALLOC_CHECK_CLIENT
        prev = prev_value;
#endif
        UNUSED(prev_value);
      }

      constexpr Iter() = default;

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
      Object::BHeadPtr<BView, BQueue> peek()
      {
        return curr;
      }

      /**
       * Moves the iterator on, and returns the current value.
       */
      template<typename Domesticator>
      Object::BHeadPtr<BView, BQueue>
      take(const FreeListKey& key, Domesticator domesticate)
      {
        auto c = curr;
        auto next = curr->read_next(key, domesticate);

        Aal::prefetch(next.unsafe_ptr());
        curr = next;
#ifdef SNMALLOC_CHECK_CLIENT
        c->check_prev(prev);
        prev = signed_prev(address_cast(c), address_cast(next), key);
#else
        UNUSED(key);
#endif
        c->cleanup();
        return c;
      }
    };

    /**
     * Used to build a free list in object space.
     *
     * Adds signing of pointers in the SNMALLOC_CHECK_CLIENT mode
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
    template<
      bool RANDOM,
      bool INIT = true,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BView = capptr::bounds::Alloc,
      SNMALLOC_CONCEPT(capptr::ConceptBound) BQueue = capptr::bounds::AllocWild>
    class Builder
    {
      static constexpr size_t LENGTH = RANDOM ? 2 : 1;

      /*
       * We use native pointers below so that we don't run afoul of strict
       * aliasing rules.  head is a Object::HeadPtr<BView, BQueue> -- that is, a
       * known-domesticated pointer to a queue of wild pointers -- and it's
       * usually the case that end is a Object::BQueuePtr<BQueue>* -- that is, a
       * known-domesticated pointer to a wild pointer to a queue of wild
       * pointers.  However, in order to do branchless inserts, we set end =
       * &head, which breaks strict aliasing rules with the types as given.
       * Fortunately, these are private members and so we can use native
       * pointers and just expose a more strongly typed interface.
       */

      // Pointer to the first element.
      std::array<void*, LENGTH> head{nullptr};
      // Pointer to the reference to the last element.
      // In the empty case end[i] == &head[i]
      // This enables branch free enqueuing.
      std::array<void**, LENGTH> end{nullptr};

      Object::BQueuePtr<BQueue>* cast_end(uint32_t ix)
      {
        return reinterpret_cast<Object::BQueuePtr<BQueue>*>(end[ix]);
      }

      void set_end(uint32_t ix, Object::BQueuePtr<BQueue>* p)
      {
        end[ix] = reinterpret_cast<void**>(p);
      }

      Object::BHeadPtr<BView, BQueue> cast_head(uint32_t ix)
      {
        return Object::BHeadPtr<BView, BQueue>(
          static_cast<Object::T<BQueue>*>(head[ix]));
      }

      std::array<uint16_t, RANDOM ? 2 : 0> length{};

    public:
      constexpr Builder()
      {
        if (INIT)
        {
          init();
        }
      }

      /**
       * Checks if the builder contains any elements.
       */
      bool empty()
      {
        for (size_t i = 0; i < LENGTH; i++)
        {
          if (end[i] != &head[i])
          {
            return false;
          }
        }
        return true;
      }

      /**
       * Adds an element to the builder
       */
      void add(
        Object::BHeadPtr<BView, BQueue> n,
        const FreeListKey& key,
        LocalEntropy& entropy)
      {
        uint32_t index;
        if constexpr (RANDOM)
          index = entropy.next_bit();
        else
          index = 0;

        set_end(index, Object::store_next(cast_end(index), n, key));
        if constexpr (RANDOM)
        {
          length[index]++;
        }
      }

      /**
       * Adds an element to the builder, if we are guaranteed that
       * RANDOM is false.  This is useful in certain construction
       * cases that do not need to introduce randomness, such as
       * during the initialisation construction of a free list, which
       * uses its own algorithm, or during building remote deallocation
       * lists, which will be randomised at the other end.
       */
      template<bool RANDOM_ = RANDOM>
      std::enable_if_t<!RANDOM_>
      add(Object::BHeadPtr<BView, BQueue> n, const FreeListKey& key)
      {
        static_assert(RANDOM_ == RANDOM, "Don't set template parameter");
        set_end(0, Object::store_next(cast_end(0), n, key));
      }

      /**
       * Makes a terminator to a free list.
       */
      SNMALLOC_FAST_PATH void
      terminate_list(uint32_t index, const FreeListKey& key)
      {
        Object::store_null(cast_end(index), key);
      }

      /**
       * Read head removing potential encoding
       *
       * Although, head does not require meta-data protection
       * as it is not stored in an object allocation. For uniformity
       * it is treated like the next_object field in a free Object
       * and is thus subject to encoding if the next_object pointers
       * encoded.
       */
      Object::BHeadPtr<BView, BQueue>
      read_head(uint32_t index, const FreeListKey& key)
      {
        return Object::decode_next(
          address_cast(&head[index]), cast_head(index), key);
      }

      address_t get_fake_signed_prev(uint32_t index, const FreeListKey& key)
      {
        return signed_prev(
          address_cast(&head[index]), address_cast(read_head(index, key)), key);
      }

      /**
       * Close a free list, and set the iterator parameter
       * to iterate it.
       *
       * In the RANDOM case, it may return only part of the freelist.
       *
       * The return value is how many entries are still contained in the
       * builder.
       */
      SNMALLOC_FAST_PATH uint16_t
      close(Iter<BView, BQueue>& fl, const FreeListKey& key)
      {
        uint32_t i;
        if constexpr (RANDOM)
        {
          SNMALLOC_ASSERT(end[1] != &head[0]);
          SNMALLOC_ASSERT(end[0] != &head[1]);

          // Select longest list.
          i = length[0] > length[1] ? 0 : 1;
        }
        else
        {
          i = 0;
        }

        terminate_list(i, key);

        fl = {read_head(i, key), get_fake_signed_prev(i, key)};

        end[i] = &head[i];

        if constexpr (RANDOM)
        {
          length[i] = 0;
          return length[1 - i];
        }
        else
        {
          return 0;
        }
      }

      /**
       * Set the builder to a not building state.
       */
      constexpr void init()
      {
        for (size_t i = 0; i < LENGTH; i++)
        {
          end[i] = &head[i];
          if (RANDOM)
          {
            length[i] = 0;
          }
        }
      }

      template<bool RANDOM_ = RANDOM>
      std::enable_if_t<
        !RANDOM_,
        std::pair<
          Object::BHeadPtr<BView, BQueue>,
          Object::BHeadPtr<BView, BQueue>>>
      extract_segment(const FreeListKey& key)
      {
        static_assert(RANDOM_ == RANDOM, "Don't set SFINAE parameter!");
        SNMALLOC_ASSERT(!empty());

        auto first = read_head(0, key);
        // end[0] is pointing to the first field in the object,
        // this is doing a CONTAINING_RECORD like cast to get back
        // to the actual object.  This isn't true if the builder is
        // empty, but you are not allowed to call this in the empty case.
        auto last =
          Object::BHeadPtr<BView, BQueue>(Object::from_next_ptr(cast_end(0)));
        init();
        return {first, last};
      }

      template<typename Domesticator>
      SNMALLOC_FAST_PATH void
      validate(const FreeListKey& key, Domesticator domesticate)
      {
#ifdef SNMALLOC_CHECK_CLIENT
        for (uint32_t i = 0; i < LENGTH; i++)
        {
          if (&head[i] == end[i])
          {
            SNMALLOC_CHECK(length[i] == 0);
            continue;
          }

          size_t count = 1;
          auto curr = read_head(i, key);
          auto prev = get_fake_signed_prev(i, key);
          while (true)
          {
            curr->check_prev(prev);
            if (address_cast(&(curr->next_object)) == address_cast(end[i]))
              break;
            count++;
            auto next = curr->read_next(key, domesticate);
            prev = signed_prev(address_cast(curr), address_cast(next), key);
            curr = next;
          }
          SNMALLOC_CHECK(count == length[i]);
        }
#else
        UNUSED(key);
        UNUSED(domesticate);
#endif
      }

      /**
       * Returns length of the shorter free list.
       *
       * This method is only usable if the free list is adding randomisation
       * as that is when it has two lists.
       */
      template<bool RANDOM_ = RANDOM>
      [[nodiscard]] std::enable_if_t<RANDOM_, size_t> min_list_length() const
      {
        static_assert(RANDOM_ == RANDOM, "Don't set SFINAE parameter!");

        return length[0] < length[1] ? length[0] : length[1];
      }
    };
  } // namespace freelist
} // namespace snmalloc
