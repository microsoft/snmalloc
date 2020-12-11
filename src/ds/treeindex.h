#include "../aal/aal.h"

#include <array>
#include <atomic>
#include <iostream>
#include <string>
#include <iomanip>

namespace snmalloc
{
  /**
   * This class provides an index of size total_entries.
   *
   * By specifying a way to allocated blocks, `alloc_block` and the block size
   * in bytes, `block_size` the index is made into a tree. `get` is wait-free
   * and has no conditionals, `set` can block if two threads are attempting to
   * add a sub-node in the tree at the same time.
   */
  template<
    typename T,
    size_t total_entries,
    void* (*alloc_block)() = nullptr,
    size_t block_size_template = 0>
  class TreeIndex
  {
    static constexpr size_t block_size = block_size_template == 0 ?
      total_entries * sizeof(T) :
      block_size_template;

    static_assert(
      (block_size_template == 0) == (alloc_block == nullptr),
      "Must set alloc_block and block_size_template parameter");

    static_assert(block_size >= sizeof(T), "Block must contain at least one T");

  public:
    static constexpr bool is_leaf = (total_entries * sizeof(T)) <= block_size;

    static_assert(
      (block_size >= sizeof(uintptr_t) * 2) || is_leaf,
      "Block must contain at least two pointers.");

    /**
     * Calculate the entries that are stored at this level of the tree.
     *
     * `TT` is used to allow the change of representation from T at the bottom
     *level to pointers at higher levels. `entries` is used to say how many
     *entries are required at this level.
     **/
    template<typename TT, size_t entries>
    static constexpr size_t calc_entries()
    {
      if constexpr (entries * sizeof(TT) <= block_size)
      {
        return entries;
      }
      else
      {
        constexpr size_t next = (entries * sizeof(TT)) / block_size;
        return calc_entries<void*, next>();
      }
    }

    /// The number of elements at this level of the tree.
    static constexpr size_t entries = calc_entries<T, total_entries>();
    /// The number of entries each sub entry should contain.
    static constexpr size_t sub_entries = total_entries / entries;

    /**
     * Type for the next level in the tree. If the current level is a leaf
     * then this is not used.
     */
    using SubT = std::conditional_t<
      is_leaf,
      TreeIndex<T, 1, nullptr, 0>,
      TreeIndex<T, sub_entries, alloc_block, block_size>>;

    /**
     * Type of entries in the tree that point to lower levels.
     * If this level is a leaf, then this type is not used.
     */
    struct Ptr
    {
      typename SubT::ArrayT* value;

      constexpr Ptr() noexcept : value(is_leaf ? nullptr : original()) {}

      Ptr(typename SubT::ArrayT* v) noexcept : value(v) {}
    };

    // Type of entries for this level of the tree
    using Entries = std::conditional_t<is_leaf, T, Ptr>;
    using ArrayT = std::array<std::atomic<Entries>, entries>;

    inline static typename SubT::ArrayT original_block{};

    constexpr static typename SubT::ArrayT* original()
    {
      if constexpr (is_leaf)
      {
        return nullptr;
      }
      else
      {
        return const_cast<typename SubT::ArrayT*>(&original_block);
      }
    };

    static_assert(original() != nullptr || is_leaf, "This must hold, or you compiler is bust.");

    // The address used for the lock at for this level in the tree.
    inline static typename SubT::ArrayT lock_block{};
    constexpr static typename SubT::ArrayT* lock()
    {
      if constexpr (is_leaf)
      {
        return nullptr;
      }
      else
      {
        return const_cast<typename SubT::ArrayT*>(&lock_block);
      }
    };

    constexpr TreeIndex() noexcept : array() {}

    /// Get element at this index.
    T get(size_t index)
    {
      return get(array, index);
    }

    static T get(ArrayT& array, size_t index)
    {
      if constexpr (is_leaf)
      {
        return array[index].load(std::memory_order_relaxed);
      }
      else
      {
        typename SubT::ArrayT* sub_array =
          array[index / sub_entries].load(std::memory_order_relaxed).value;
        SNMALLOC_ASSERT(sub_array != nullptr);
        return SubT::get(*sub_array, index % sub_entries);
      }
    }

    /// Set element at this index.
    void set(size_t index, T v)
    {
      set(array, index, v);
    }

    static void set(ArrayT& array, size_t index, T v)
    {
      if constexpr (is_leaf)
      {
        array[index] = v;
      }
      else
      {
        auto sub_array =
          array[index / sub_entries].load(std::memory_order_relaxed).value;
        if ((sub_array != original()) && (sub_array != lock()))
        {
          SNMALLOC_ASSERT(sub_array != nullptr);
          SubT::set(*sub_array, index % sub_entries, v);
          return;
        }
        set_slow(array, index, v);
      }
    }

    /**
     * Returns the pointer to the leaf entry associated with this index.
     * If the entry was not already in the tree creates a unique entry
     * for it.
     */
    std::atomic<T>* get_addr(size_t index)
    {
      return get_addr(array, index);
    }

    static std::atomic<T>* get_addr(ArrayT& array, size_t index)
    {
      if constexpr (is_leaf)
      {
        return &(array[index / sub_entries]);
      }
      else
      {
        auto expected = Ptr(original());
        if (array[index / sub_entries].compare_exchange_strong(
              expected, Ptr(lock())))
        {
          // Allocate new TreeIndex
          void* new_block = alloc_block();
          // Initialise the block
          auto new_block_typed = new (new_block) typename SubT::ArrayT();
          // Unlock TreeIndex
          array[index / sub_entries].store(
            Ptr(new_block_typed), std::memory_order_relaxed);
        }
        while (
          array[index / sub_entries].load(std::memory_order_relaxed).value ==
          lock())
        {
          Aal::pause();
        }

        auto sub_array =
          array[index / sub_entries].load(std::memory_order_relaxed).value;
        SNMALLOC_ASSERT(sub_array != nullptr);
        return SubT::get_addr(*sub_array, index % sub_entries);
      }
    }

    static void initial_invariant(ArrayT* array, std::string path)
    {
      if constexpr (is_leaf)
      {
        UNUSED(array);
        return;
      }
      else
      {
        for (size_t i = 0; i < entries; i++)
        {
          if ((*array)[i].load().value != original())
          {
            std::cout << "Error " << path << "[" << i << "] = " << (*array)[i].load().value << std::endl;
            return;
          }
        }
        
        SubT::initial_invariant(original(), path + "::original");
        SubT::initial_invariant(lock(), path + "::lock");
      }
    }

    void initial_invariant()
    {
      return initial_invariant(&array, "root");
    }

  private:
    static void set_slow(ArrayT& array, size_t index, T v)
    {
      auto p = get_addr(array, index);
      p->store(v, std::memory_order_relaxed);
    }

    /**
     *  Data stored for this level of the tree.
     */
    ArrayT array;
  };
} // namespace snmalloc