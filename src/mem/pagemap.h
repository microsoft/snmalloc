#pragma once

#include "../ds/bits.h"
#include "../ds/helpers.h"

#include <atomic>
#include <utility>

namespace snmalloc
{
  static constexpr size_t PAGEMAP_NODE_BITS = 16;
  static constexpr size_t PAGEMAP_NODE_SIZE = 1ULL << PAGEMAP_NODE_BITS;

  /**
   * Structure describing the configuration of a pagemap.  When querying a
   * pagemap from a different instantiation of snmalloc, the pagemap is exposed
   * as a `void*`.  This structure allows the caller to check whether the
   * pagemap is of the format that they expect.
   */
  struct PagemapConfig
  {
    /**
     * The version of the pagemap structure.  This is always 1 in existing
     * versions of snmalloc.  This will be incremented every time the format
     * changes in an incompatible way.  Changes to the format may add fields to
     * the end of this structure.
     */
    uint32_t version;
    /**
     * Is this a flat pagemap?  If this field is false, the pagemap is the
     * hierarchical structure.
     */
    bool is_flat_pagemap;
    /**
     * Number of bytes in a pointer.
     */
    uint8_t sizeof_pointer;
    /**
     * The number of bits of the address used to index into the pagemap.
     */
    uint64_t pagemap_bits;
    /**
     * The size (in bytes) of a pagemap entry.
     */
    size_t size_of_entry;
  };

  template<size_t GRANULARITY_BITS, typename T, T default_content>
  class Pagemap
  {
  private:
    static constexpr size_t COVERED_BITS =
      bits::ADDRESS_BITS - GRANULARITY_BITS;
    static constexpr size_t CONTENT_BITS =
      bits::next_pow2_bits_const(sizeof(T));

    static_assert(
      PAGEMAP_NODE_BITS - CONTENT_BITS < COVERED_BITS,
      "Should use the FlatPageMap as it does not require a tree");

    static constexpr size_t BITS_FOR_LEAF = PAGEMAP_NODE_BITS - CONTENT_BITS;
    static constexpr size_t ENTRIES_PER_LEAF = 1 << BITS_FOR_LEAF;
    static constexpr size_t LEAF_MASK = ENTRIES_PER_LEAF - 1;

    static constexpr size_t BITS_PER_INDEX_LEVEL =
      PAGEMAP_NODE_BITS - POINTER_BITS;
    static constexpr size_t ENTRIES_PER_INDEX_LEVEL = 1 << BITS_PER_INDEX_LEVEL;
    static constexpr size_t ENTRIES_MASK = ENTRIES_PER_INDEX_LEVEL - 1;

    static constexpr size_t INDEX_BITS =
      BITS_FOR_LEAF > COVERED_BITS ? 0 : COVERED_BITS - BITS_FOR_LEAF;

    static constexpr size_t INDEX_LEVELS = INDEX_BITS / BITS_PER_INDEX_LEVEL;
    static constexpr size_t TOPLEVEL_BITS =
      INDEX_BITS - (INDEX_LEVELS * BITS_PER_INDEX_LEVEL);
    static constexpr size_t TOPLEVEL_ENTRIES = 1 << TOPLEVEL_BITS;
    static constexpr size_t TOPLEVEL_SHIFT =
      (INDEX_LEVELS * BITS_PER_INDEX_LEVEL) + BITS_FOR_LEAF + GRANULARITY_BITS;

    // Value used to represent when a node is being added too
    static constexpr InvalidPointer<1> LOCKED_ENTRY{};

    struct Leaf
    {
      std::atomic<T> values[ENTRIES_PER_LEAF];
    };

    struct PagemapEntry
    {
      std::atomic<PagemapEntry*> entries[ENTRIES_PER_INDEX_LEVEL];
    };

    static_assert(
      sizeof(PagemapEntry) == sizeof(Leaf), "Should be the same size");

    static_assert(
      sizeof(PagemapEntry) == PAGEMAP_NODE_SIZE, "Should be the same size");

    // Init removed as not required as this is only ever a global
    // cl is generating a memset of zero, which will be a problem
    // in libc/ucrt bring up.  On ucrt this will run after the first
    // allocation.
    //  TODO: This is fragile that it is not being memset, and we should review
    //  to ensure we don't get bitten by this in the future.
    std::atomic<PagemapEntry*> top[TOPLEVEL_ENTRIES]; // = {nullptr};

    template<bool create_addr>
    inline PagemapEntry* get_node(std::atomic<PagemapEntry*>* e, bool& result)
    {
      // The page map nodes are all allocated directly from the OS zero
      // initialised with a system call.  We don't need any ordered to guarantee
      // to see that correctly.
      PagemapEntry* value = e->load(std::memory_order_relaxed);

      if ((value == nullptr) || (value == LOCKED_ENTRY))
      {
        if constexpr (create_addr)
        {
          value = nullptr;

          if (e->compare_exchange_strong(
                value, LOCKED_ENTRY, std::memory_order_relaxed))
          {
            auto& v = default_memory_provider;
            value = v.alloc_chunk<PagemapEntry, OS_PAGE_SIZE>();
            e->store(value, std::memory_order_release);
          }
          else
          {
            while (address_cast(e->load(std::memory_order_relaxed)) ==
                   LOCKED_ENTRY)
            {
              bits::pause();
            }
            value = e->load(std::memory_order_acquire);
          }
        }
        else
        {
          result = false;
          return nullptr;
        }
      }
      result = true;
      return value;
    }

    template<bool create_addr>
    inline std::pair<Leaf*, size_t> get_leaf_index(uintptr_t addr, bool& result)
    {
#ifdef FreeBSD_KERNEL
      // Zero the top 16 bits - kernel addresses all have them set, but the
      // data structure assumes that they're zero.
      addr &= 0xffffffffffffULL;
#endif
      size_t ix = addr >> TOPLEVEL_SHIFT;
      size_t shift = TOPLEVEL_SHIFT;
      std::atomic<PagemapEntry*>* e = &top[ix];

      for (size_t i = 0; i < INDEX_LEVELS; i++)
      {
        PagemapEntry* value = get_node<create_addr>(e, result);
        if (!result)
          return std::pair(nullptr, 0);

        shift -= BITS_PER_INDEX_LEVEL;
        ix = (addr >> shift) & ENTRIES_MASK;
        e = &value->entries[ix];

        if constexpr (INDEX_LEVELS == 1)
        {
          UNUSED(i);
          break;
        }
        i++;

        if (i == INDEX_LEVELS)
          break;
      }

      Leaf* leaf = reinterpret_cast<Leaf*>(get_node<create_addr>(e, result));

      if (!result)
        return std::pair(nullptr, 0);

      shift -= BITS_FOR_LEAF;
      ix = (addr >> shift) & LEAF_MASK;
      return std::pair(leaf, ix);
    }

    template<bool create_addr>
    inline std::atomic<T>* get_addr(uintptr_t p, bool& success)
    {
      auto leaf_ix = get_leaf_index<create_addr>(p, success);
      return &(leaf_ix.first->values[leaf_ix.second]);
    }

    std::atomic<T>* get_ptr(uintptr_t p)
    {
      bool success;
      return get_addr<true>(p, success);
    }

  public:
    /**
     * The pagemap configuration describing this instantiation of the template.
     */
    static constexpr PagemapConfig config = {
      1, false, sizeof(uintptr_t), GRANULARITY_BITS, sizeof(T)};

    /**
     * Cast a `void*` to a pointer to this template instantiation, given a
     * config describing the configuration.  Return null if the configuration
     * passed does not correspond to this template instantiation.
     *
     * This intended to allow code that depends on the pagemap having a
     * specific representation to fail gracefully.
     */
    static Pagemap* cast_to_pagemap(void* pm, const PagemapConfig* c)
    {
      if (
        (c->version != 1) || (c->is_flat_pagemap) ||
        (c->sizeof_pointer != sizeof(uintptr_t)) ||
        (c->pagemap_bits != GRANULARITY_BITS) ||
        (c->size_of_entry != sizeof(T)) || (!std::is_integral_v<T>))
      {
        return nullptr;
      }
      return static_cast<Pagemap*>(pm);
    }

    /**
     * Returns the index of a pagemap entry within a given page.  This is used
     * in code that propagates changes to the pagemap elsewhere.
     */
    size_t index_for_address(uintptr_t p)
    {
      bool success;
      return (OS_PAGE_SIZE - 1) &
        reinterpret_cast<size_t>(get_addr<true>(p, success));
    }

    /**
     * Returns the address of the page containing
     */
    void* page_for_address(uintptr_t p)
    {
      bool success;
      return reinterpret_cast<void*>(
        ~(OS_PAGE_SIZE - 1) &
        reinterpret_cast<uintptr_t>(get_addr<true>(p, success)));
    }

    T get(uintptr_t p)
    {
      bool success;
      auto addr = get_addr<false>(p, success);
      if (!success)
        return default_content;
      return addr->load(std::memory_order_relaxed);
    }

    void set(uintptr_t p, T x)
    {
      bool success;
      auto addr = get_addr<true>(p, success);
      addr->store(x, std::memory_order_relaxed);
    }

    void set_range(uintptr_t p, T x, size_t length)
    {
      bool success;
      do
      {
        auto leaf_ix = get_leaf_index<true>(p, success);
        size_t ix = leaf_ix.second;

        auto last = bits::min(LEAF_MASK + 1, ix + length);

        auto diff = last - ix;

        for (; ix < last; ix++)
        {
          SNMALLOC_ASSUME(leaf_ix.first != nullptr);
          leaf_ix.first->values[ix] = x;
        }

        length = length - diff;
        p = p + (diff << GRANULARITY_BITS);
      } while (length > 0);
    }
  };

  /**
   * Simple pagemap that for each GRANULARITY_BITS of the address range
   * stores a T.
   **/
  template<size_t GRANULARITY_BITS, typename T>
  class FlatPagemap
  {
  private:
    static constexpr size_t COVERED_BITS =
      bits::ADDRESS_BITS - GRANULARITY_BITS;
    static constexpr size_t CONTENT_BITS =
      bits::next_pow2_bits_const(sizeof(T));
    static constexpr size_t ENTRIES = 1ULL << (COVERED_BITS + CONTENT_BITS);
    static constexpr size_t SHIFT = GRANULARITY_BITS;

    std::atomic<T> top[ENTRIES];

  public:
    /**
     * The pagemap configuration describing this instantiation of the template.
     */
    static constexpr PagemapConfig config = {
      1, true, sizeof(uintptr_t), GRANULARITY_BITS, sizeof(T)};

    /**
     * Cast a `void*` to a pointer to this template instantiation, given a
     * config describing the configuration.  Return null if the configuration
     * passed does not correspond to this template instantiation.
     *
     * This intended to allow code that depends on the pagemap having a
     * specific representation to fail gracefully.
     */
    static FlatPagemap* cast_to_pagemap(void* pm, const PagemapConfig* c)
    {
      if (
        (c->version != 1) || (!c->is_flat_pagemap) ||
        (c->sizeof_pointer != sizeof(uintptr_t)) ||
        (c->pagemap_bits != GRANULARITY_BITS) ||
        (c->size_of_entry != sizeof(T)) || (!std::is_integral_v<T>))
      {
        return nullptr;
      }
      return static_cast<FlatPagemap*>(pm);
    }

    T get(uintptr_t p)
    {
      return top[p >> SHIFT].load(std::memory_order_relaxed);
    }

    void set(uintptr_t p, T x)
    {
      top[p >> SHIFT].store(x, std::memory_order_relaxed);
    }

    void set_range(uintptr_t p, T x, size_t length)
    {
      size_t index = p >> SHIFT;
      do
      {
        top[index].store(x, std::memory_order_relaxed);
        index++;
        length--;
      } while (length > 0);
    }
  };
} // namespace snmalloc
