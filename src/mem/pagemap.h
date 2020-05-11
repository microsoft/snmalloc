#pragma once

#include "../ds/bits.h"
#include "../ds/helpers.h"
#include "../ds/treeindex.h"

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

  /**
   * Simple pagemap that for each GRANULARITY_BITS of the address range
   * stores a T.
   */
  template<size_t GRANULARITY_BITS, typename T>
  class alignas(OS_PAGE_SIZE) PagemapV2
  {
  private:
    static_assert(
      sizeof(T) == bits::next_pow2_const(sizeof(T)),
      "Needed for good performance.");
    static constexpr size_t COVERED_BITS =
      bits::ADDRESS_BITS - GRANULARITY_BITS;
    static constexpr size_t CONTENT_BITS =
      bits::next_pow2_bits_const(sizeof(T));
    static constexpr size_t ENTRIES = 1ULL << COVERED_BITS;
    static constexpr size_t SHIFT = GRANULARITY_BITS;

    static void* alloc_block()
    {
      return default_memory_provider()
        .alloc_chunk_untyped<PAGEMAP_NODE_SIZE>();
    }

    using TreeIndexType = std::conditional_t<
      pal_supports<LazyCommit>,
      snmalloc::TreeIndex<T, ENTRIES>,
      snmalloc::TreeIndex<T, ENTRIES, alloc_block, PAGEMAP_NODE_SIZE>>;

    TreeIndexType tree_index{};

  public:
    /**
     * The pagemap configuration describing this instantiation of the template.
     */
    static constexpr PagemapConfig config = {2,
                                             TreeIndexType::is_leaf,
                                             sizeof(uintptr_t),
                                             GRANULARITY_BITS,
                                             sizeof(T)};

    /**
     * Cast a `void*` to a pointer to this template instantiation, given a
     * config describing the configuration.  Return null if the configuration
     * passed does not correspond to this template instantiation.
     *
     * This intended to allow code that depends on the pagemap having a
     * specific representation to fail gracefully.
     */
    static PagemapV2* cast_to_pagemap(void* pm, const PagemapConfig* c)
    {
      if (
        (c->version != 2) || (c->is_flat_pagemap != TreeIndexType::is_leaf) ||
        (c->sizeof_pointer != sizeof(uintptr_t)) ||
        (c->pagemap_bits != GRANULARITY_BITS) ||
        (c->size_of_entry != sizeof(T)) || (!std::is_integral_v<T>))
      {
        return nullptr;
      }
      return static_cast<PagemapV2*>(pm);
    }

    T get(uintptr_t p)
    {
      return tree_index.get(p >> SHIFT);
    }

    void set(uintptr_t p, T x)
    {
      tree_index.set(p >> SHIFT, x);
    }

    void set_range(uintptr_t p, T x, size_t length)
    {
      size_t index = p >> SHIFT;
      do
      {
        tree_index.set(index, x);
        index++;
        length--;
      } while (length > 0);
    }

    /**
     * Returns the index within a page for the specified address.
     */
    size_t index_for_address(uintptr_t p)
    {
      return bits::align_down(static_cast<size_t>(p) >> SHIFT, OS_PAGE_SIZE);
    }

    /**
     * Returns the address of the page containing the pagemap address p.
     */
    void* page_for_address(uintptr_t p)
    {
      SNMALLOC_ASSERT(
        (reinterpret_cast<uintptr_t>(tree_index.get_addr(0)) &
         (OS_PAGE_SIZE - 1)) == 0);
      return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(tree_index.get_addr(p >> SHIFT)) &
        ~(OS_PAGE_SIZE - 1));
    }
  };
} // namespace snmalloc
