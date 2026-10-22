#include "test/opt.h"
#include "test/setup.h"
#include "test/xoroshiro.h"

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <snmalloc/snmalloc.h>
#include <thread>
#include <vector>

using namespace snmalloc;

namespace
{
  struct Slot
  {
    void* allocation;
    size_t owner;
    bool replaced;
  };

  struct PoolStats
  {
    size_t count = 0;
    size_t in_use = 0;
    size_t pending = 0;
  };

  PoolStats pool_stats()
  {
    PoolStats result;
    auto* alloc = AllocPool<Config>::iterate();
    while (alloc != nullptr)
    {
      result.count++;
      result.in_use += alloc->debug_is_in_use() ? 1 : 0;
      result.pending += alloc->debug_has_pending_remote() ? 1 : 0;
      alloc = AllocPool<Config>::iterate(alloc);
    }
    return result;
  }

  void wait_until(const std::atomic<size_t>& value, size_t target)
  {
    while (value.load(std::memory_order_acquire) < target)
      std::this_thread::yield();
  }

  size_t latency_bucket(uint64_t nanoseconds)
  {
    size_t bucket = 0;
    while ((nanoseconds > 1) && (bucket < 63))
    {
      nanoseconds >>= 1;
      bucket++;
    }
    return bucket;
  }
}

int main(int argc, char** argv)
{
  setup();
  opt::Opt opt(argc, argv);

  const bool smoke = opt.has("--smoke");
  const size_t builder_count = opt.is<size_t>("--builders", smoke ? 4 : 8);
  const size_t table_size =
    opt.is<size_t>("--table", smoke ? (1 << 15) : (1 << 18));

  if ((builder_count == 0) || (table_size < builder_count))
  {
    std::cerr << "builders must be non-zero and no larger than table size"
              << std::endl;
    return 1;
  }

  const size_t requested_size = opt.is<size_t>("--size", 1024);
  const size_t overlap = opt.is<size_t>("--overlap", table_size / 32);
  const size_t termination_gap =
    opt.is<size_t>("--termination-gap", table_size / (8 * builder_count));
  const size_t survivor_churn = opt.is<size_t>("--survivor-churn", table_size);
  const size_t minimum_churn_ms = opt.is<size_t>("--minimum-churn-ms", 0);
  using SchedulingState = AllocPoolAssistance<Config>;

  if (requested_size > MAX_SMALL_SIZECLASS_SIZE)
  {
    std::cerr << "size must use a small sizeclass" << std::endl;
    return 1;
  }

  std::vector<Slot> table(table_size);
  std::vector<std::atomic<bool>> builder_exit(builder_count);
  std::vector<std::atomic<bool>> builder_released(builder_count);
  std::vector<size_t> replaced_before_release(builder_count);
  std::vector<size_t> replaced_after_release(builder_count);
  std::vector<RemoteAllocator*> builder_queues(builder_count);

  std::atomic<size_t> builders_ready{0};
  std::atomic<bool> churn_ready{false};
  std::atomic<bool> start_churn{false};
  std::atomic<size_t> churn_operations{0};
  std::atomic<size_t> first_replacements{0};
  std::atomic<bool> stop_churn{false};
  std::array<size_t, 64> allocation_latency_histogram{};
  uint64_t maximum_allocation_latency_ns = 0;
  uint64_t churn_elapsed_ns = 0;
  uint64_t pending_cleared_ns = 0;

  for (size_t i = 0; i < builder_count; i++)
  {
    builder_exit[i] = false;
    builder_released[i] = false;
  }

  std::thread churn_thread([&]() {
    void* initial = snmalloc::alloc(1);
    snmalloc::dealloc(initial);
    churn_ready.store(true, std::memory_order_release);

    while (!start_churn.load(std::memory_order_acquire))
      std::this_thread::yield();

    xoroshiro::p128r32 random(0x5eed, 0x1234);
    size_t remaining = table_size;
    size_t survivor_count = 0;
    bool observed_pending = false;
    bool minimum_time_complete = minimum_churn_ms == 0;
    const auto churn_start = std::chrono::steady_clock::now();

    while ((remaining != 0) || (survivor_count < survivor_churn) ||
           !stop_churn.load(std::memory_order_acquire) ||
           !minimum_time_complete)
    {
      const size_t index = random.next() % table_size;
      auto& slot = table[index];
      const auto allocation_start = std::chrono::steady_clock::now();
      void* replacement = snmalloc::alloc(requested_size);
      const auto allocation_end = std::chrono::steady_clock::now();
      const auto allocation_latency_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          allocation_end - allocation_start)
          .count());
      allocation_latency_histogram[latency_bucket(allocation_latency_ns)]++;
      if (allocation_latency_ns > maximum_allocation_latency_ns)
        maximum_allocation_latency_ns = allocation_latency_ns;
      snmalloc::dealloc(slot.allocation);
      slot.allocation = replacement;

      if (!slot.replaced)
      {
        slot.replaced = true;
        remaining--;
        first_replacements.fetch_add(1, std::memory_order_release);
        if (builder_released[slot.owner].load(std::memory_order_acquire))
          replaced_after_release[slot.owner]++;
        else
          replaced_before_release[slot.owner]++;
      }
      else if (remaining == 0)
      {
        survivor_count++;
      }

      const size_t operation =
        churn_operations.fetch_add(1, std::memory_order_release) + 1;
      if ((operation & 1023) == 0)
      {
        const auto pending = SchedulingState::debug_pending_count();
        observed_pending |= pending > 0;
        if (observed_pending && (pending <= 0) && (pending_cleared_ns == 0))
        {
          pending_cleared_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
              allocation_end - churn_start)
              .count());
        }
      }
      if (!minimum_time_complete && ((operation & 1023) == 0))
      {
        const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
            allocation_end - churn_start);
        minimum_time_complete =
          elapsed.count() >= static_cast<int64_t>(minimum_churn_ms);
      }
    }

    churn_elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - churn_start)
        .count());
  });

  std::vector<std::thread> builders;
  builders.reserve(builder_count);
  for (size_t builder = 0; builder < builder_count; builder++)
  {
    builders.emplace_back([&, builder]() {
      for (size_t index = builder; index < table_size; index += builder_count)
      {
        void* allocation = snmalloc::alloc(requested_size);
        table[index] = {allocation, builder, false};
        if (builder_queues[builder] == nullptr)
        {
          const auto& entry =
            Config::Backend::get_metaentry(address_cast(allocation));
          builder_queues[builder] = entry.get_remote();
        }
      }

      builders_ready.fetch_add(1, std::memory_order_acq_rel);
      while (!builder_exit[builder].load(std::memory_order_acquire))
        std::this_thread::yield();
    });
  }

  wait_until(builders_ready, builder_count);
  while (!churn_ready.load(std::memory_order_acquire))
    std::this_thread::yield();

  for (size_t i = 0; i < builder_count; i++)
  {
    if (builder_queues[i] == nullptr)
    {
      std::cerr << "builder did not allocate" << std::endl;
      return 1;
    }
    for (size_t j = 0; j < i; j++)
    {
      if (builder_queues[i] == builder_queues[j])
      {
        std::cerr << "builders did not retain distinct allocators" << std::endl;
        return 1;
      }
    }
  }

  const size_t usable_size = snmalloc::alloc_size(table[0].allocation);
  const size_t slab_size =
    sizeclass_to_slab_size(size_to_sizeclass(requested_size));
  const size_t allocator_count = builder_count + 1;
  const size_t retained_slab_tolerance = allocator_count * slab_size;
  const size_t remote_batch_tolerance =
    allocator_count * static_cast<size_t>(REMOTE_CACHE);
  const size_t metadata_tolerance = allocator_count * MIN_CHUNK_SIZE;
  const size_t retention_tolerance =
    retained_slab_tolerance + remote_batch_tolerance + metadata_tolerance;
  const size_t phase1_current = Config::Backend::get_current_usage();
  const size_t phase1_peak = Config::Backend::get_peak_usage();

  start_churn.store(true, std::memory_order_release);
  wait_until(churn_operations, overlap);

  for (size_t builder = 0; builder < builder_count; builder++)
  {
    builder_exit[builder].store(true, std::memory_order_release);
    builders[builder].join();
    builder_released[builder].store(true, std::memory_order_release);
    wait_until(churn_operations, overlap + ((builder + 1) * termination_gap));
  }

  wait_until(first_replacements, table_size);
  wait_until(churn_operations, overlap + table_size + survivor_churn);
  stop_churn.store(true, std::memory_order_release);
  churn_thread.join();

  const auto stats = pool_stats();
  const size_t final_current = Config::Backend::get_current_usage();
  const size_t final_peak = Config::Backend::get_peak_usage();
  const auto inactive_pending_count = SchedulingState::debug_pending_count();

  const size_t measured_operations = churn_operations.load();
  const size_t p99_target = ((measured_operations * 99) + 99) / 100;
  size_t p99_bucket = 0;
  size_t cumulative_latency_samples = 0;
  for (; p99_bucket < allocation_latency_histogram.size(); p99_bucket++)
  {
    cumulative_latency_samples += allocation_latency_histogram[p99_bucket];
    if (cumulative_latency_samples >= p99_target)
      break;
  }
  const uint64_t allocation_p99_upper_ns =
    p99_bucket == 63 ? UINT64_MAX : (uint64_t{1} << (p99_bucket + 1));
  const uint64_t churn_operations_per_second = churn_elapsed_ns == 0 ?
    0 :
    static_cast<uint64_t>(
      static_cast<double>(measured_operations) * 1000000000.0 /
      static_cast<double>(churn_elapsed_ns));

  size_t post_release_replacements = 0;
  for (size_t builder = 0; builder < builder_count; builder++)
  {
    post_release_replacements += replaced_after_release[builder];
    std::cout << "builder[" << builder << "] first_replacements_before_release="
              << replaced_before_release[builder]
              << " first_replacements_after_release="
              << replaced_after_release[builder] << std::endl;
  }

  const size_t expected_retained_payload =
    post_release_replacements * usable_size;
  if (expected_retained_payload <= (4 * retention_tolerance))
  {
    std::cerr << "retained-payload signal is too small for tolerance"
              << std::endl;
    return 1;
  }

  std::cout << "builders=" << builder_count << " table=" << table_size
            << " requested_size=" << requested_size
            << " usable_size=" << usable_size
            << " live_payload=" << (table_size * requested_size)
            << " expected_retained_payload=" << expected_retained_payload
            << " retained_slab_tolerance=" << retained_slab_tolerance
            << " remote_batch_tolerance=" << remote_batch_tolerance
            << " metadata_tolerance=" << metadata_tolerance
            << " retention_tolerance=" << retention_tolerance
            << " phase1_current=" << phase1_current
            << " phase1_peak=" << phase1_peak
            << " final_current=" << final_current
            << " final_peak=" << final_peak << " pool_size=" << stats.count
            << " pool_in_use=" << stats.in_use
            << " pooled_pending_queues=" << stats.pending
            << " inactive_pending_count=" << inactive_pending_count
            << " pending_cleared_ns=" << pending_cleared_ns
            << " churn_operations=" << measured_operations
            << " churn_elapsed_ns=" << churn_elapsed_ns
            << " churn_operations_per_second=" << churn_operations_per_second
            << " allocation_p99_upper_ns=" << allocation_p99_upper_ns
            << " maximum_allocation_latency_ns="
            << maximum_allocation_latency_ns << std::endl;

  for (auto& slot : table)
    snmalloc::dealloc(slot.allocation);
  cleanup_unused<Config>();
  cleanup_unused<Config>();
  debug_check_empty<Config>();
  return 0;
}
