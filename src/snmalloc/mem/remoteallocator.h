#pragma once

#include "freelist_queue.h"

namespace snmalloc
{
  class RemoteMessageAssertions;

  /**
   * Entries on a remote message queue.  Logically, this is a pair of freelist
   * linkages, together with some metadata:
   *
   * - a cyclic list ("ring") of free objects (atypically for rings, there is
   *   no sentinel node here: the message itself is a free object),
   *
   * - the length of that ring
   *
   * - the linkage for the message queue itself
   *
   * In practice, there is a fair bit more going on here: the ring of free
   * objects is not entirely encoded as a freelist.  While traversing the
   * successor pointers in objects on the ring will eventually lead back to
   * this RemoteMessage object, the successor pointer from this object is
   * encoded as a relative displacement.  This is guaranteed to be physically
   * smaller than a full pointe (because slabs are smaller than the whole
   * address space).  This gives us enough room to pack in the length of the
   * ring, without needing to grow the structure.
   */
  class RemoteMessage
  {
    friend class RemoteMessageAssertions;

    freelist::Object::T<> free_ring;
    freelist::Object::T<> message_link;

    static_assert(
      sizeof(free_ring.next_object) >= sizeof(void*),
      "RemoteMessage bitpacking needs sizeof(void*) in next_object");

    static auto decode_next(uintptr_t encoded, capptr::Alloc<RemoteMessage> m)
    {
      return capptr_rewild(
               pointer_offset_signed(
                 m, static_cast<ptrdiff_t>(encoded) >> MAX_CAPACITY_BITS))
        .as_static<freelist::Object::T<>>();
    }

  public:
    static auto emplace_in_alloc(capptr::Alloc<void> alloc)
    {
      return CapPtr<RemoteMessage, capptr::bounds::Alloc>::unsafe_from(
        new (alloc.unsafe_ptr()) RemoteMessage());
    }

    static auto mk_from_freelist_builder(
      freelist::Builder<false, true>& flb,
      const FreeListKey& key,
      address_t key_tweak)
    {
      size_t size = flb.extract_segment_length();

      SNMALLOC_ASSERT(size < bits::one_at_bit(MAX_CAPACITY_BITS));

      auto [first, last] = flb.extract_segment(key, key_tweak);

      // Preserve the last node's backpointer and change its type.
      auto last_prev = last->prev;
      auto self = CapPtr<RemoteMessage, capptr::bounds::Alloc>::unsafe_from(
        new (last.unsafe_ptr()) RemoteMessage());
      self->free_ring.prev = last_prev;

      // XXX CHERI

      auto n = freelist::HeadPtr::unsafe_from(
        unsafe_from_uintptr<freelist::Object::T<>>(
          (static_cast<uintptr_t>(pointer_diff_signed(self, first))
           << MAX_CAPACITY_BITS) +
          size));

      // Close the ring, storing our bit-packed value in the next field.
      freelist::Object::store_nextish(
        &self->free_ring.next_object, first, key, key_tweak, n);

      return self;
    }

    static freelist::HeadPtr to_message_link(capptr::Alloc<RemoteMessage> m)
    {
      return pointer_offset(m, offsetof(RemoteMessage, message_link))
        .as_reinterpret<freelist::Object::T<>>();
    }

    static capptr::Alloc<RemoteMessage>
    from_message_link(freelist::HeadPtr chainPtr)
    {
      return pointer_offset_signed(
               chainPtr,
               -static_cast<ptrdiff_t>(offsetof(RemoteMessage, message_link)))
        .as_reinterpret<RemoteMessage>();
    }

    template<typename Domesticator_queue>
    static std::pair<freelist::HeadPtr, uint16_t> open_free_ring(
      capptr::Alloc<RemoteMessage> m,
      const FreeListKey& key,
      address_t key_tweak,
      Domesticator_queue domesticate)
    {
      uintptr_t encoded =
        m->free_ring.read_next(key, key_tweak, domesticate).unsafe_uintptr();

      uint16_t decoded_size =
        static_cast<uint16_t>(encoded) & bits::mask_bits(MAX_CAPACITY_BITS);
      static_assert(sizeof(decoded_size) * 8 > MAX_CAPACITY_BITS);

      auto next = domesticate(decode_next(encoded, m));

      if constexpr (mitigations(freelist_backward_edge))
      {
        next->check_prev(
          signed_prev(address_cast(m), address_cast(next), key, key_tweak));
      }
      else
      {
        UNUSED(key);
        UNUSED(key_tweak);
      }

      // XXX CHERI

      return {next.template as_static<freelist::Object::T<>>(), decoded_size};
    }
  };

  class RemoteMessageAssertions
  {
    static_assert(sizeof(RemoteMessage) <= MIN_ALLOC_SIZE);
    static_assert(offsetof(RemoteMessage, free_ring) == 0);

    static_assert(
      MAX_SLAB_SPAN_BITS + MAX_CAPACITY_BITS < 8 * sizeof(void*),
      "Ring bit-stuffing trick can't reach far enough to enclose a slab");
  };

  /**
   * A RemoteAllocator is the message queue of freed objects.  It builds on the
   * FreeListMPSCQ but encapsulates knowledge that the objects are actually
   * RemoteMessage-s and not just any freelist::object::T<>s.
   *
   * RemoteAllocator-s may be exposed to client tampering.  As a result,
   * pointer domestication may be necessary.  See the documentation for
   * FreeListMPSCQ for details.
   */
  struct RemoteAllocator
  {
    /**
     * Global key for all remote lists.
     *
     * Note that we use a single key for all remote free lists and queues.
     * This is so that we do not have to recode next pointers when sending
     * segments, and look up specific keys based on destination.  This is
     * potentially more performant, but could make it easier to guess the key.
     */
    inline static FreeListKey key_global{0xdeadbeef, 0xbeefdead, 0xdeadbeef};

    FreeListMPSCQ<key_global> list;

    using alloc_id_t = address_t;

    constexpr RemoteAllocator() = default;

    void invariant()
    {
      list.invariant();
    }

    void init()
    {
      list.init();
    }

    template<typename Domesticator_queue, typename Cb>
    void destroy_and_iterate(Domesticator_queue domesticate, Cb cb)
    {
      auto cbwrap = [cb](freelist::HeadPtr p) SNMALLOC_FAST_PATH_LAMBDA {
        cb(RemoteMessage::from_message_link(p));
      };

      return list.destroy_and_iterate(domesticate, cbwrap);
    }

    template<typename Domesticator_head, typename Domesticator_queue>
    inline bool can_dequeue(
      Domesticator_head domesticate_head, Domesticator_queue domesticate_queue)
    {
      return list.can_dequeue(domesticate_head, domesticate_queue);
    }

    /**
     * Pushes a list of messages to the queue. Each message from first to
     * last should be linked together through their next pointers.
     *
     * The Domesticator here is used only on pointers read from the head.  See
     * the commentary on the class.
     */
    template<typename Domesticator_head>
    void enqueue(
      capptr::Alloc<RemoteMessage> first,
      capptr::Alloc<RemoteMessage> last,
      Domesticator_head domesticate_head)
    {
      list.enqueue(
        RemoteMessage::to_message_link(first),
        RemoteMessage::to_message_link(last),
        domesticate_head);
    }

    /**
     * Destructively iterate the queue.  Each queue element is removed and fed
     * to the callback in turn.  The callback may return false to stop iteration
     * early (but must have processed the element it was given!).
     *
     * Takes a domestication callback for each of "pointers read from head" and
     * "pointers read from queue".  See the commentary on the class.
     */
    template<
      typename Domesticator_head,
      typename Domesticator_queue,
      typename Cb>
    void dequeue(
      Domesticator_head domesticate_head,
      Domesticator_queue domesticate_queue,
      Cb cb)
    {
      auto cbwrap = [cb](freelist::HeadPtr p) SNMALLOC_FAST_PATH_LAMBDA {
        return cb(RemoteMessage::from_message_link(p));
      };
      list.dequeue(domesticate_head, domesticate_queue, cbwrap);
    }

    alloc_id_t trunc_id()
    {
      return address_cast(this);
    }
  };
} // namespace snmalloc
