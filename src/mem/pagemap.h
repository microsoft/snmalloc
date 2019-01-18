#pragma once

#include "../ds/bits.h"

#include <algorithm>
#include <atomic>

namespace snmalloc
{
  static constexpr size_t PAGEMAP_NODE_BITS = 16;
  static constexpr size_t PAGEMAP_NODE_SIZE = 1ULL << PAGEMAP_NODE_BITS;

  template<size_t GRANULARITY_BITS, typename T, T default_content>
  class Pagemap
  {
  private:
    static constexpr size_t COVERED_BITS =
      bits::ADDRESS_BITS - GRANULARITY_BITS;
    static constexpr size_t POINTER_BITS =
      bits::next_pow2_bits_const(sizeof(void*));
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
    static constexpr uintptr_t LOCKED_ENTRY = 1;

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

      if ((uintptr_t)value <= LOCKED_ENTRY)
      {
        if constexpr (create_addr)
        {
          value = nullptr;

          if (e->compare_exchange_strong(
                value, (PagemapEntry*)LOCKED_ENTRY, std::memory_order_relaxed))
          {
            auto& v = default_memory_provider;
            value =
              (PagemapEntry*)v.alloc_chunk<OS_PAGE_SIZE>(PAGEMAP_NODE_SIZE);
            e->store(value, std::memory_order_release);
          }
          else
          {
            while ((uintptr_t)e->load(std::memory_order_relaxed) ==
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
    inline std::pair<Leaf*, size_t> get_leaf_index(void* p, bool& result)
    {
      size_t addr = (size_t)p;
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

      Leaf* leaf = (Leaf*)get_node<create_addr>(e, result);

      if (!result)
        return std::pair(nullptr, 0);

      shift -= BITS_FOR_LEAF;
      ix = (addr >> shift) & LEAF_MASK;
      return std::pair(leaf, ix);
    }

    template<bool create_addr>
    inline std::atomic<T>* get_addr(void* p, bool& success)
    {
      auto leaf_ix = get_leaf_index<create_addr>(p, success);
      return &(leaf_ix.first->values[leaf_ix.second]);
    }

    std::atomic<T>* get_ptr(void* p)
    {
      bool success;
      return get_addr<true>(p, success);
    }

  public:
    /**
     * Returns the index of a pagemap entry within a given page.  This is used
     * in code that propagates changes to the pagemap elsewhere.
     */
    size_t index_for_address(void* p)
    {
      bool success;
      return (OS_PAGE_SIZE - 1) &
        reinterpret_cast<size_t>(get_addr<true>(p, success));
    }

    /**
     * Returns the address of the page containing
     */
    void* page_for_address(void* p)
    {
      bool success;
      return reinterpret_cast<void*>(
        ~(OS_PAGE_SIZE - 1) &
        reinterpret_cast<uintptr_t>(get_addr<true>(p, success)));
    }

    T get(void* p)
    {
      bool success;
      auto addr = get_addr<false>(p, success);
      if (!success)
        return default_content;
      return addr->load(std::memory_order_relaxed);
    }

    void set(void* p, T x)
    {
      bool success;
      auto addr = get_addr<true>(p, success);
      addr->store(x, std::memory_order_relaxed);
    }

    void set_range(void* p, T x, size_t length)
    {
      bool success;
      do
      {
        auto leaf_ix = get_leaf_index<true>(p, success);
        size_t ix = leaf_ix.second;

        auto last = std::min(LEAF_MASK + 1, ix + length);

        auto diff = last - ix;

        for (; ix < last; ix++)
        {
          leaf_ix.first->values[ix] = x;
        }

        length = length - diff;
        p = (void*)((uintptr_t)p + (diff << GRANULARITY_BITS));
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
    T get(void* p)
    {
      return top[(size_t)p >> SHIFT].load(std::memory_order_relaxed);
    }

    void set(void* p, T x)
    {
      top[(size_t)p >> SHIFT].store(x, std::memory_order_relaxed);
    }

    void set_range(void* p, T x, size_t length)
    {
      size_t index = (size_t)p >> SHIFT;
      do
      {
        top[index].store(x, std::memory_order_relaxed);
        index++;
        length--;
      } while (length > 0);
    }
  };
}
