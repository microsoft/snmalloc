#pragma once

#include "../ds/mpmcstack.h"
#include "../mem/metaslab.h"
#include "../mem/sizeclasstable.h"
#include "../pal/pal_ds.h"

#ifdef SNMALLOC_TRACING
#  include <iostream>
#endif

#include <new>

namespace snmalloc
{
  /**
   * Used to store slabs in the unused sizes.
   */
  struct ChunkRecord
  {
    MetaCommon meta_common;
    std::atomic<ChunkRecord*> next;
  };
  static_assert(std::is_standard_layout_v<ChunkRecord>);
  static_assert(
    offsetof(ChunkRecord, meta_common) == 0,
    "ChunkRecord and Metaslab must share a common prefix");

  /**
   * How many slab sizes that can be provided.
   */
  constexpr size_t NUM_SLAB_SIZES = Pal::address_bits - MIN_CHUNK_BITS;

  /**
   * Used to ensure the per slab meta data is large enough for both use cases.
   */
  static_assert(
    sizeof(Metaslab) >= sizeof(ChunkRecord), "We conflate these two types.");

  /**
   * Number of free stacks per chunk size that each allocator will use.
   * For performance ideally a power of 2.  We will return to the central
   * pool anything that has not be used in the last NUM_EPOCHS - 1, where
   * each epoch is separated by DecayMemoryTimerObject::PERIOD.
   * I.e. if period is 500ms and num of epochs is 4, then we will return to
   * the central pool anything not used for the last 1500-2000ms.
   */
  constexpr size_t NUM_EPOCHS = 4;
  static_assert(bits::is_pow2(NUM_EPOCHS), "Code assumes power of two.");

  class ChunkAllocatorLocalState
  {
    friend class ChunkAllocator;

    /**
     * Stack of slabs that have been returned for reuse.
     */
    ModArray<
      NUM_SLAB_SIZES,
      ModArray<NUM_EPOCHS, MPMCStack<ChunkRecord, RequiresInit>>>
      chunk_stack;

    /**
     * Used for list of all ChunkAllocatorLocalStates.
     */
    std::atomic<ChunkAllocatorLocalState*> next{nullptr};
  };

  /**
   * This is the global state required for the chunk allocator.
   * It must be provided as a part of the shared state handle
   * to the chunk allocator.
   */
  class ChunkAllocatorState
  {
    friend class ChunkAllocator;
    /**
     * Stack of slabs that have been returned for reuse.
     */
    ModArray<NUM_SLAB_SIZES, MPMCStack<ChunkRecord, RequiresInit>>
      decommitted_chunk_stack;

    /**
     * All memory issued by this address space manager
     */
    std::atomic<size_t> peak_memory_usage_{0};

    std::atomic<size_t> memory_in_stacks{0};

    std::atomic<ChunkAllocatorLocalState*> all_local{nullptr};

    /**
     * Which is the current epoch to place dealloced chunks, and the
     * first place we look for allocating chunks.
     */
    std::atomic<size_t> epoch{0};

    // Flag to ensure one-shot registration with the PAL for notifications.
    std::atomic_flag register_decay{};

  public:
    size_t unused_memory()
    {
      return memory_in_stacks;
    }

    size_t peak_memory_usage()
    {
      return peak_memory_usage_;
    }

    void add_peak_memory_usage(size_t size)
    {
      peak_memory_usage_ += size;
#ifdef SNMALLOC_TRACING
      std::cout << "peak_memory_usage_: " << peak_memory_usage_ << std::endl;
#endif
    }
  };

  class ChunkAllocator
  {
    template<SNMALLOC_CONCEPT(ConceptPAL) Pal>
    class DecayMemoryTimerObject : public PalTimerObject
    {
      ChunkAllocatorState* state;

      /***
       * Method for callback object to perform lazy decommit.
       */
      static void process(PalTimerObject* p)
      {
        // Unsafe downcast here. Don't want vtable and RTTI.
        auto self = reinterpret_cast<DecayMemoryTimerObject*>(p);
        ChunkAllocator::handle_decay_tick(self->state);
      }

      // Specify that we notify the ChunkAllocator every 500ms.
      static constexpr size_t PERIOD = 500;

    public:
      DecayMemoryTimerObject(ChunkAllocatorState* state)
      : PalTimerObject(&process, PERIOD), state(state)
      {}
    };

    static void handle_decay_tick(ChunkAllocatorState* state)
    {
      auto new_epoch = (state->epoch + 1) % NUM_EPOCHS;
      // Flush old index for all threads.
      ChunkAllocatorLocalState* curr = state->all_local;
      while (curr != nullptr)
      {
        for (size_t sc = 0; sc < NUM_SLAB_SIZES; sc++)
        {
          auto& old_stack = curr->chunk_stack[sc][new_epoch];
          ChunkRecord* record = old_stack.pop_all();
          while (record != nullptr)
          {
            auto next = record->next.load();

            // Disable pages for this
            Pal::notify_not_using(
              record->meta_common.chunk.unsafe_ptr(),
              slab_sizeclass_to_size(sc));

            // Add to global state
            state->decommitted_chunk_stack[sc].push(record);

            record = next;
          }
        }
        curr = curr->next;
      }

      // Advance current index
      state->epoch = new_epoch;
    }

  public:
    template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
    static std::pair<capptr::Chunk<void>, Metaslab*> alloc_chunk(
      typename SharedStateHandle::LocalState& local_state,
      ChunkAllocatorLocalState& chunk_alloc_local_state,
      sizeclass_t sizeclass,
      sizeclass_t slab_sizeclass, // TODO sizeclass_t
      size_t slab_size,
      RemoteAllocator* remote)
    {
      ChunkAllocatorState& state =
        SharedStateHandle::get_chunk_allocator_state(&local_state);

      if (slab_sizeclass >= NUM_SLAB_SIZES)
      {
        // Your address space is not big enough for this allocation!
        return {nullptr, nullptr};
      }

      ChunkRecord* chunk_record = nullptr;
      // Try local cache of chunks first
      for (size_t e = 0; e < NUM_EPOCHS && chunk_record == nullptr; e++)
      {
        chunk_record =
          chunk_alloc_local_state
            .chunk_stack[slab_sizeclass][(state.epoch - e) % NUM_EPOCHS]
            .pop();
      }

      // Try global cache.
      if (chunk_record == nullptr)
      {
        chunk_record = state.decommitted_chunk_stack[slab_sizeclass].pop();
        if (chunk_record != nullptr)
          Pal::notify_using<NoZero>(
            chunk_record->meta_common.chunk.unsafe_ptr(), slab_size);
      }

      if (chunk_record != nullptr)
      {
        auto slab = chunk_record->meta_common.chunk;
        state.memory_in_stacks -= slab_size;
        auto meta = reinterpret_cast<Metaslab*>(chunk_record);
#ifdef SNMALLOC_TRACING
        std::cout << "Reuse slab:" << slab.unsafe_ptr() << " slab_sizeclass "
                  << slab_sizeclass << " size " << slab_size
                  << " memory in stacks " << state.memory_in_stacks
                  << std::endl;
#endif
        MetaEntry entry{meta, remote, sizeclass};
        SharedStateHandle::Pagemap::set_metaentry(
          &local_state, address_cast(slab), slab_size, entry);
        return {slab, meta};
      }

      // Allocate a fresh slab as there are no available ones.
      // First create meta-data
      auto [slab, meta] = SharedStateHandle::alloc_chunk(
        &local_state, slab_size, remote, sizeclass);
#ifdef SNMALLOC_TRACING
      std::cout << "Create slab:" << slab.unsafe_ptr() << " slab_sizeclass "
                << slab_sizeclass << " size " << slab_size << std::endl;
#endif

      state.add_peak_memory_usage(slab_size);
      state.add_peak_memory_usage(sizeof(Metaslab));
      // TODO handle bounded versus lazy pagemaps in stats
      state.add_peak_memory_usage(
        (slab_size / MIN_CHUNK_SIZE) * sizeof(MetaEntry));

      return {slab, meta};
    }

    template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
    SNMALLOC_SLOW_PATH static void dealloc(
      typename SharedStateHandle::LocalState& local_state,
      ChunkAllocatorLocalState& chunk_alloc_local_state,
      ChunkRecord* p,
      size_t slab_sizeclass)
    {
      ChunkAllocatorState& state =
        SharedStateHandle::get_chunk_allocator_state(&local_state);
#ifdef SNMALLOC_TRACING
      std::cout << "Return slab:" << p->meta_common.chunk.unsafe_ptr()
                << " slab_sizeclass " << slab_sizeclass << " size "
                << slab_sizeclass_to_size(slab_sizeclass)
                << " memory in stacks " << state.memory_in_stacks << std::endl;
#endif

      chunk_alloc_local_state.chunk_stack[slab_sizeclass][state.epoch].push(p);

      state.memory_in_stacks += slab_sizeclass_to_size(slab_sizeclass);
    }

    /**
     * Provide a block of meta-data with size and align.
     *
     * Backend allocator may use guard pages and separate area of
     * address space to protect this from corruption.
     */
    template<
      typename U,
      SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle,
      typename... Args>
    static U* alloc_meta_data(
      typename SharedStateHandle::LocalState* local_state, Args&&... args)
    {
      // Cache line align
      size_t size = bits::align_up(sizeof(U), 64);

      capptr::Chunk<void> p =
        SharedStateHandle::template alloc_meta_data<U>(local_state, size);

      if (p == nullptr)
        return nullptr;

      return new (p.unsafe_ptr()) U(std::forward<Args>(args)...);
    }

    template<SNMALLOC_CONCEPT(ConceptBackendGlobals) SharedStateHandle>
    static void register_local_state(
      typename SharedStateHandle::LocalState& local_state,
      ChunkAllocatorLocalState& chunk_alloc_local_state)
    {
      ChunkAllocatorState& state =
        SharedStateHandle::get_chunk_allocator_state(&local_state);

      // Register with the Pal to receive notifications.
      if (!state.register_decay.test_and_set())
      {
        auto timer = alloc_meta_data<
          DecayMemoryTimerObject<typename SharedStateHandle::Pal>,
          SharedStateHandle>(&local_state, &state);
        if (timer != nullptr)
        {
          SharedStateHandle::Pal::register_timer(timer);
        }
        else
        {
          // We failed to register the notification.
          // This is not catarophic, but if we can't allocate this
          // state something else will fail shortly.
          state.register_decay.clear();
        }
      }

      // Add to the list of local states.
      auto* head = state.all_local.load();
      do
      {
        chunk_alloc_local_state.next = head;
      } while (!state.all_local.compare_exchange_strong(
        head, &chunk_alloc_local_state));
    }
  };
} // namespace snmalloc
