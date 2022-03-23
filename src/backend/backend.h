#pragma once
#include "../mem/allocconfig.h"
#include "../pal/pal.h"
#include "chunkallocator.h"
#include "commitrange.h"
#include "commonconfig.h"
#include "empty_range.h"
#include "globalrange.h"
#include "largebuddyrange.h"
#include "metatypes.h"
#include "pagemap.h"
#include "pagemapregisterrange.h"
#include "palrange.h"
#include "range_helpers.h"
#include "smallbuddyrange.h"
#include "statsrange.h"
#include "subrange.h"

#if defined(SNMALLOC_CHECK_CLIENT) && !defined(OPEN_ENCLAVE)
/**
 * Protect meta data blocks by allocating separate from chunks for
 * user allocations. This involves leaving gaps in address space.
 * This is less efficient, so should only be applied for the checked
 * build.
 *
 * On Open Enclave the address space is limited, so we disable this
 * feature.
 */
#  define SNMALLOC_META_PROTECTED
#endif

namespace snmalloc
{
  /**
   * This class implements the standard backend for handling allocations.
   * It abstracts page table management and address space management.
   */
  template<
    SNMALLOC_CONCEPT(ConceptPAL) PAL,
    bool fixed_range,
    typename PageMapEntry = MetaEntry>
  class BackendAllocator : public CommonConfig
  {
  public:
    using Pal = PAL;

    class Pagemap
    {
      friend class BackendAllocator;

      SNMALLOC_REQUIRE_CONSTINIT
      static inline FlatPagemap<MIN_CHUNK_BITS, PageMapEntry, PAL, fixed_range>
        concretePagemap;

    public:
      /**
       * Get the metadata associated with a chunk.
       *
       * Set template parameter to true if it not an error
       * to access a location that is not backed by a chunk.
       */
      template<typename Ret = MetaEntry, bool potentially_out_of_range = false>
      SNMALLOC_FAST_PATH static const Ret& get_metaentry(address_t p)
      {
        static_assert(
          std::is_base_of_v<MetaEntry, Ret> && sizeof(MetaEntry) == sizeof(Ret),
          "Backend Pagemap get_metaentry return must look like MetaEntry");
        return static_cast<const Ret&>(
          concretePagemap.template get<potentially_out_of_range>(p));
      }

      /**
       * Get the metadata associated with a chunk.
       *
       * Set template parameter to true if it not an error
       * to access a location that is not backed by a chunk.
       */
      template<bool potentially_out_of_range = false>
      SNMALLOC_FAST_PATH static MetaEntry& get_metaentry_mut(address_t p)
      {
        return concretePagemap.template get_mut<potentially_out_of_range>(p);
      }

      /**
       * Set the metadata associated with a chunk.
       */
      SNMALLOC_FAST_PATH
      static void set_metaentry(address_t p, size_t size, const MetaEntry& t)
      {
        for (address_t a = p; a < p + size; a += MIN_CHUNK_SIZE)
        {
          concretePagemap.set(a, t);
        }
      }

      static void register_range(address_t p, size_t sz)
      {
        concretePagemap.register_range(p, sz);
      }

      /**
       * Return the bounds of the memory this back-end manages as a pair of
       * addresses (start then end).  This is available iff this is a
       * fixed-range Backend.
       */
      template<bool fixed_range_ = fixed_range>
      static SNMALLOC_FAST_PATH
        std::enable_if_t<fixed_range_, std::pair<address_t, address_t>>
        get_bounds()
      {
        static_assert(
          fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

        return concretePagemap.get_bounds();
      }

      static bool is_initialised()
      {
        return concretePagemap.is_initialised();
      }
    };

#if defined(_WIN32) || defined(__CHERI_PURE_CAPABILITY__)
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = false;
#else
    static constexpr bool CONSOLIDATE_PAL_ALLOCS = true;
#endif

#if defined(OPEN_ENCLAVE)
    // Single global buddy allocator is used on open enclave due to
    // the limited address space.
    using StatsR = StatsRange<SmallBuddyRange<
      LargeBuddyRange<EmptyRange, bits::BITS - 1, bits::BITS - 1, Pagemap>>>;
    using GlobalR = GlobalRange<StatsR>;
    using ObjectRange = GlobalR;
    using GlobalMetaRange = ObjectRange;
#else
    // Set up source of memory
    using P = PalRange<DefaultPal>;
    using Base = std::
      conditional_t<fixed_range, EmptyRange, PagemapRegisterRange<Pagemap, P>>;
    // Global range of memory
    using StatsR = StatsRange<LargeBuddyRange<
      Base,
      24,
      bits::BITS - 1,
      Pagemap,
      CONSOLIDATE_PAL_ALLOCS>>;
    using GlobalR = GlobalRange<StatsR>;

#  ifdef SNMALLOC_META_PROTECTED
    // Source for object allocations
    using ObjectRange =
      LargeBuddyRange<CommitRange<GlobalR, DefaultPal>, 21, 21, Pagemap>;
    // Set up protected range for metadata
    using SubR = CommitRange<SubRange<GlobalR, DefaultPal, 6>, DefaultPal>;
    using MetaRange =
      SmallBuddyRange<LargeBuddyRange<SubR, 21 - 6, bits::BITS - 1, Pagemap>>;
    using GlobalMetaRange = GlobalRange<MetaRange>;
#  else
    // Source for object allocations and metadata
    // No separation between the two
    using ObjectRange = SmallBuddyRange<
      LargeBuddyRange<CommitRange<GlobalR, DefaultPal>, 21, 21, Pagemap>>;
    using GlobalMetaRange = GlobalRange<ObjectRange>;
#  endif
#endif

    struct LocalState
    {
      typename ObjectRange::State object_range;

#ifdef SNMALLOC_META_PROTECTED
      typename MetaRange::State meta_range;

      typename MetaRange::State& get_meta_range()
      {
        return meta_range;
      }
#else
      typename ObjectRange::State& get_meta_range()
      {
        return object_range;
      }
#endif
    };

  public:
    template<bool fixed_range_ = fixed_range>
    static std::enable_if_t<!fixed_range_> init()
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      Pagemap::concretePagemap.init();
    }

    template<bool fixed_range_ = fixed_range>
    static std::enable_if_t<fixed_range_> init(void* base, size_t length)
    {
      static_assert(fixed_range_ == fixed_range, "Don't set SFINAE parameter!");

      auto [heap_base, heap_length] =
        Pagemap::concretePagemap.init(base, length);

      Pagemap::register_range(address_cast(heap_base), heap_length);

      // Push memory into the global range.
      range_to_pow_2_blocks<MIN_CHUNK_BITS>(
        capptr::Chunk<void>(heap_base),
        heap_length,
        [&](capptr::Chunk<void> p, size_t sz, bool) {
          typename GlobalR::State g;
          g->dealloc_range(p, sz);
        });
    }

    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     *
     * The template argument is the type of the metadata being allocated.  This
     * allows the backend to allocate different types of metadata in different
     * places or with different policies.  The default implementation, here,
     * does not avail itself of this degree of freedom.
     */
    template<typename T>
    static capptr::Chunk<void>
    alloc_meta_data(LocalState* local_state, size_t size)
    {
      capptr::Chunk<void> p;
      if (local_state != nullptr)
      {
        p = local_state->get_meta_range()->alloc_range_with_leftover(size);
      }
      else
      {
        typename GlobalMetaRange::State global_state;
        p = global_state->alloc_range(bits::next_pow2(size));
      }

      if (p == nullptr)
        errno = ENOMEM;

      return p;
    }

    /**
     * Returns a chunk of memory with alignment and size of `size`, and a
     * metaslab block.
     *
     * It additionally set the meta-data for this chunk of memory to
     * be
     *   (remote, sizeclass, metaslab)
     * where metaslab, is the second element of the pair return.
     */
    static std::pair<capptr::Chunk<void>, Metaslab*>
    alloc_chunk(LocalState& local_state, size_t size, uintptr_t ras)
    {
      SNMALLOC_ASSERT(bits::is_pow2(size));
      SNMALLOC_ASSERT(size >= MIN_CHUNK_SIZE);

      SNMALLOC_ASSERT((ras & MetaEntry::REMOTE_BACKEND_MARKER) == 0);
      ras &= ~MetaEntry::REMOTE_BACKEND_MARKER;

      auto meta_cap =
        local_state.get_meta_range()->alloc_range(PAGEMAP_METADATA_STRUCT_SIZE);

      auto meta = meta_cap.template as_reinterpret<Metaslab>().unsafe_ptr();

      if (meta == nullptr)
      {
        errno = ENOMEM;
        return {nullptr, nullptr};
      }

      auto p = local_state.object_range->alloc_range(size);

#ifdef SNMALLOC_TRACING
      std::cout << "Alloc chunk: " << p.unsafe_ptr() << " (" << size << ")"
                << std::endl;
#endif
      if (p == nullptr)
      {
        local_state.get_meta_range()->dealloc_range(
          meta_cap, PAGEMAP_METADATA_STRUCT_SIZE);
        errno = ENOMEM;
#ifdef SNMALLOC_TRACING
        std::cout << "Out of memory" << std::endl;
#endif
        return {p, nullptr};
      }

      meta->meta_common.chunk = p;

      MetaEntry t(&meta->meta_common, ras);
      Pagemap::set_metaentry(address_cast(p), size, t);

      p = Aal::capptr_bound<void, capptr::bounds::Chunk>(p, size);
      return {p, meta};
    }

    static void dealloc_chunk(
      LocalState& local_state, ChunkRecord* chunk_record, size_t size)
    {
      auto chunk = chunk_record->meta_common.chunk;

      /*
       * The backend takes possession of these chunks now, by disassociating
       * any existing remote allocator and metadata structure.  If
       * interrogated, the sizeclass reported by the MetaEntry is 0, which has
       * size 0.
       */
      MetaEntry t(nullptr, MetaEntry::REMOTE_BACKEND_MARKER);
      Pagemap::set_metaentry(address_cast(chunk), size, t);

      local_state.get_meta_range()->dealloc_range(
        capptr::Chunk<void>(chunk_record), PAGEMAP_METADATA_STRUCT_SIZE);

      local_state.object_range->dealloc_range(chunk, size);
    }

    static size_t get_current_usage()
    {
      typename StatsR::State stats_state;
      return stats_state->get_current_usage();
    }

    static size_t get_peak_usage()
    {
      typename StatsR::State stats_state;
      return stats_state->get_peak_usage();
    }
  };
} // namespace snmalloc
