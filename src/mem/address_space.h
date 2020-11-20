#include "../ds/address.h"
#include "../ds/flaglock.h"
#include "../pal/pal.h"
#include "pagemap.h"

#include <array>
namespace snmalloc
{
  template<
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    SNMALLOC_CONCEPT(ConceptAAL) AAL,
    typename PrimAlloc>
  class AuthMap
  {
    /*
     * Without AlignedAllocation, we (below) adopt a fallback mechanism that
     * over-allocates and then finds an aligned region within the too-large
     * region.  The "trimmings" from either side are also registered in hopes
     * that they can be used for later allocations.
     *
     * Unfortunately, that strategy does not work for this AuthMap: trimmings
     * may be smaller than the granularity of our backing PageMap, and so
     * we would be unable to amplify authority.  Eventually we may arrive at
     * a need for an AuthMap that is compatible with this approach, but for
     * the moment it's far simpler to assume that we can always ask for
     * memory sufficiently aligned to cover an entire PageMap granule.
     */
    static_assert(
      !aal_supports<StrictProvenance> || pal_supports<AlignedAllocation, PAL>,
      "StrictProvenance requires platform support for aligned allocation");

    struct default_alloc_size_t
    {
      static constexpr size_t ptrauth_root_alloc_size = 1;
    };

  public:
    /*
     * Compute the block allocation size to use for AlignedAllocations.  This
     * is either PAL::ptrauth_root_alloc_size, on architectures that require
     * StrictProvenance, or the placeholder from above.
     */
    static constexpr size_t alloc_size = std::conditional_t<
      aal_supports<StrictProvenance, AAL>,
      PAL,
      default_alloc_size_t>::ptrauth_root_alloc_size;

    /*
     * Because we assume that we can `ptrauth_amplify` and then
     * `Superslab::get()` on the result to get to the Superslab metadata
     * headers, it must be the case that provenance roots cover entire
     * Superslabs.
     */
    static_assert(
      !aal_supports<StrictProvenance> ||
        ((alloc_size > 0) && (alloc_size % SUPERSLAB_SIZE == 0)),
      "Provenance root granule must encompass whole superslabs");

  private:
    static constexpr size_t AUTHMAP_BITS =
      bits::next_pow2_bits_const(alloc_size);

    static constexpr bool AUTHMAP_USE_FLATPAGEMAP = pal_supports<LazyCommit> ||
      (PAGEMAP_NODE_SIZE >= sizeof(FlatPagemap<AUTHMAP_BITS, void*>));

    struct default_auth_pagemap
    {
      template<typename T>
      static SNMALLOC_FAST_PATH AuthPtr<T> get(address_t a)
      {
        UNUSED(a);
        return nullptr;
      }
    };

    using AuthPagemap = std::conditional_t<
      aal_supports<StrictProvenance, AAL>,
      std::conditional_t<
        AUTHMAP_USE_FLATPAGEMAP,
        FlatPagemap<AUTHMAP_BITS, void*>,
        Pagemap<AUTHMAP_BITS, void*, nullptr, PrimAlloc>>,
      default_auth_pagemap>;

    AuthPagemap authmap_pagemap;

  public:
    void register_root(void* root)
    {
      if constexpr (aal_supports<StrictProvenance, AAL>)
      {
        authmap_pagemap.set(address_cast(root), root);
      }
      else
      {
        UNUSED(root);
      }
    }

    template<typename T = void>
    SNMALLOC_FAST_PATH AuthPtr<T> ptrauth_amplify(ReturnPtr r)
    {
      return AAL::ptrauth_rebound(
        authmap_pagemap.template get<T>(address_cast(r.unsafe_return_ptr)), r);
    }
  };

  /**
   * Implements a power of two allocator, where all blocks are aligned to the
   * same power of two as their size. This is what snmalloc uses to get
   * alignment of very large sizeclasses.
   *
   * It cannot unreserve memory, so this does not require the
   * usual complexity of a buddy allocator.
   */
  template<SNMALLOC_CONCEPT(ConceptPAL) PAL, typename PrimAlloc>
  class AddressSpaceManager
  {
    /**
     * Stores the blocks of address space
     *
     * The first level of array indexes based on power of two size.
     *
     * The first entry ranges[n][0] is just a pointer to an address range
     * of size 2^n.
     *
     * The second entry ranges[n][1] is a pointer to a linked list of blocks
     * of this size. The final block in the list is not committed, so we commit
     * on pop for this corner case.
     *
     * Invariants
     *  ranges[n][1] != nullptr => ranges[n][0] != nullptr
     *
     * bits::BITS is used for simplicity, we do not use below the pointer size,
     * and large entries will be unlikely to be supported by the platform.
     */
    std::array<std::array<void*, 2>, bits::BITS> ranges = {};

    /**
     * This is infrequently used code, a spin lock simplifies the code
     * considerably, and should never be on the fast path.
     */
    std::atomic_flag spin_lock = ATOMIC_FLAG_INIT;

    /**
     * Checks a block satisfies its invariant.
     */
    inline void check_block(void* base, size_t align_bits)
    {
      SNMALLOC_ASSERT(
        base == pointer_align_up(base, bits::one_at_bit(align_bits)));
      // All blocks need to be bigger than a pointer.
      SNMALLOC_ASSERT(bits::one_at_bit(align_bits) >= sizeof(void*));
      UNUSED(base);
      UNUSED(align_bits);
    }

    /**
     * Adds a block to `ranges`.
     */
    void add_block(size_t align_bits, void* base)
    {
      check_block(base, align_bits);
      SNMALLOC_ASSERT(align_bits < 64);
      if (ranges[align_bits][0] == nullptr)
      {
        // Prefer first slot if available.
        ranges[align_bits][0] = base;
        return;
      }

      if (ranges[align_bits][1] != nullptr)
      {
        // Add to linked list.
        commit_block(base, sizeof(void*));
        *reinterpret_cast<void**>(base) = ranges[align_bits][1];
        check_block(ranges[align_bits][1], align_bits);
      }

      // Update head of list
      ranges[align_bits][1] = base;
      check_block(ranges[align_bits][1], align_bits);
    }

    /**
     * Find a block of the correct size. May split larger blocks
     * to satisfy this request.
     */
    void* remove_block(size_t align_bits)
    {
      auto first = ranges[align_bits][0];
      if (first == nullptr)
      {
        if (align_bits == (bits::BITS - 1))
        {
          // Out of memory
          return nullptr;
        }

        // Look for larger block and split up recursively
        void* bigger = remove_block(align_bits + 1);
        if (bigger != nullptr)
        {
          void* left_over =
            pointer_offset(bigger, bits::one_at_bit(align_bits));
          ranges[align_bits][0] = left_over;
          check_block(left_over, align_bits);
        }
        check_block(bigger, align_bits + 1);
        return bigger;
      }

      auto second = ranges[align_bits][1];
      if (second != nullptr)
      {
        commit_block(second, sizeof(void*));
        auto next = *reinterpret_cast<void**>(second);
        ranges[align_bits][1] = next;
        // Zero memory. Client assumes memory contains only zeros.
        *reinterpret_cast<void**>(second) = nullptr;
        check_block(second, align_bits);
        check_block(next, align_bits);
        return second;
      }

      check_block(first, align_bits);
      ranges[align_bits][0] = nullptr;
      return first;
    }

    /**
     * Add a range of memory to the address space.
     * Divides blocks into power of two sizes with natural alignment
     */
    void add_range(void* base, size_t length)
    {
      // Find the minimum set of maximally aligned blocks in this range.
      // Each block's alignment and size are equal.
      while (length >= sizeof(void*))
      {
        size_t base_align_bits = bits::ctz(address_cast(base));
        size_t length_align_bits = (bits::BITS - 1) - bits::clz(length);
        size_t align_bits = bits::min(base_align_bits, length_align_bits);
        size_t align = bits::one_at_bit(align_bits);

        check_block(base, align_bits);
        add_block(align_bits, base);

        base = pointer_offset(base, align);
        length -= align;
      }
    }

    /**
     * Commit a block of memory
     */
    void commit_block(void* base, size_t size)
    {
      // Rounding required for sub-page allocations.
      auto page_start = pointer_align_down<OS_PAGE_SIZE, char>(base);
      auto page_end =
        pointer_align_up<OS_PAGE_SIZE, char>(pointer_offset(base, size));
      PAL::template notify_using<NoZero>(
        page_start, static_cast<size_t>(page_end - page_start));
    }

    AuthMap<PAL, Aal, PrimAlloc> auth_map = {};

  public:
    /**
     * Returns a pointer to a block of memory of the supplied size.
     * The block will be committed, if specified by the template parameter.
     * The returned block is guaranteed to be aligened to the size.
     *
     * Only request 2^n sizes, and not less than a pointer.
     */
    template<bool committed>
    void* reserve(size_t size)
    {
      SNMALLOC_ASSERT(bits::next_pow2(size) == size);
      SNMALLOC_ASSERT(size >= sizeof(void*));

      /*
       * For sufficiently large allocations with platforms that support aligned
       * allocations and architectures that don't require StrictProvenance,
       * try asking the platform first.
       */
      if constexpr (
        pal_supports<AlignedAllocation, PAL> && !aal_supports<StrictProvenance>)
      {
        if (size >= PAL::minimum_alloc_size)
          return PAL::template reserve_aligned<committed>(size);
      }

      void* res;
      {
        FlagLock lock(spin_lock);
        res = remove_block(bits::next_pow2_bits(size));
        if (res == nullptr)
        {
          // Allocation failed ask OS for more memory
          void* block;
          size_t block_size;
          if constexpr (pal_supports<AlignedAllocation, PAL>)
          {
            /*
             * aal_supports<StrictProvenance> ends up here, too, and we ensure
             * that we always allocate whole AuthMap granules.
             */
            if constexpr (aal_supports<StrictProvenance>)
            {
              static_assert(
                !aal_supports<StrictProvenance> ||
                  (auth_map.alloc_size >= PAL::minimum_alloc_size),
                "Provenance root granule must be at least PAL's "
                "minimum_alloc_size");
              block_size = align_up(size, auth_map.alloc_size);
            }
            else
            {
              /*
               * We will have handled the case where size >= minimum_alloc_size
               * above, so we are left to handle only small things here.
               */
              block_size = PAL::minimum_alloc_size;
            }
            block = PAL::template reserve_aligned<false>(block_size);
          }
          else
          {
            // Need at least 2 times the space to guarantee alignment.
            // Hold lock here as a race could cause additional requests to
            // the PAL, and this could lead to suprious OOM.  This is
            // particularly bad if the PAL gives all the memory on first call.
            auto block_and_size = PAL::reserve_at_least(size * 2);
            block = block_and_size.first;
            block_size = block_and_size.second;

            // Ensure block is pointer aligned.
            if (
              pointer_align_up(block, sizeof(void*)) != block ||
              bits::align_up(block_size, sizeof(void*)) > block_size)
            {
              auto diff =
                pointer_diff(block, pointer_align_up(block, sizeof(void*)));
              block_size = block_size - diff;
              block_size = bits::align_down(block_size, sizeof(void*));
            }
          }
          if (block == nullptr)
          {
            return nullptr;
          }
          add_range(block, block_size);
          if constexpr (aal_supports<StrictProvenance>)
          {
            do
            {
              auth_map.register_root(block);
              block = pointer_offset(block, auth_map.alloc_size);
              block_size -= auth_map.alloc_size;
            } while (block_size > 0);
          }

          // still holding lock so guaranteed to succeed.
          res = remove_block(bits::next_pow2_bits(size));
        }
      }

      // Don't need lock while committing pages.
      if constexpr (committed)
        commit_block(res, size);

      return res;
    }

    template<typename T = void>
    SNMALLOC_FAST_PATH AuthPtr<T> ptrauth_amplify(ReturnPtr r)
    {
      return auth_map.ptrauth_amplify(r);
    }
  };
} // namespace snmalloc
