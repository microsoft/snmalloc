// Tests for Sampler and SampledList (Phase 2 of heap profiling).
// SNMALLOC_PROFILE is defined here so these classes compile standalone,
// independent of the build-level flag.
#ifndef SNMALLOC_PROFILE
#  define SNMALLOC_PROFILE 1
#endif

#include "snmalloc/mem/sampled_list.h"
#include "snmalloc/mem/sampler.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace snmalloc;

// ---------------------------------------------------------------------------
// Sampler tests
// ---------------------------------------------------------------------------

// Verify that the sampled fraction converges to 1 - e^(-size/interval).
static void test_sampler_rate()
{
  std::cout << "test_sampler_rate\n";

  Sampler s;
  g_sample_interval.store(512, stl::memory_order_relaxed);

  const size_t alloc_size = 64;
  const size_t n = 100'000;
  size_t hits = 0;

  for (size_t i = 0; i < n; i++)
    if (s.record(alloc_size) != 0)
      hits++;

  double expected_p = 1.0 - std::exp(-static_cast<double>(alloc_size) / 512.0);
  double actual_p = static_cast<double>(hits) / static_cast<double>(n);

  // Allow 5% absolute tolerance — statistical, not a tight bound.
  double error = std::abs(actual_p - expected_p);
  if (error > 0.05)
  {
    std::cerr << "sampler rate out of range: expected ~" << expected_p
              << " got " << actual_p << "\n";
    abort();
  }
}

// With interval=0, record() must never fire.
static void test_sampler_disabled()
{
  std::cout << "test_sampler_disabled\n";

  Sampler s;
  g_sample_interval.store(0, stl::memory_order_relaxed);

  for (size_t i = 0; i < 100'000; i++)
  {
    if (s.record(64) != 0)
    {
      std::cerr << "sampler fired with interval=0\n";
      abort();
    }
  }
}

// Large allocation (size >> interval) should almost always be sampled.
static void test_sampler_large_alloc()
{
  std::cout << "test_sampler_large_alloc\n";

  Sampler s;
  g_sample_interval.store(512, stl::memory_order_relaxed);

  const size_t large = 64 * 1024;
  const size_t n = 1'000;
  size_t hits = 0;

  for (size_t i = 0; i < n; i++)
    if (s.record(large) != 0)
      hits++;

  // P = 1 - e^(-64K/512) ≈ 1.0 — expect at least 95% sampled.
  if (hits < n * 95 / 100)
  {
    std::cerr << "large alloc under-sampled: " << hits << "/" << n << "\n";
    abort();
  }
}

// ---------------------------------------------------------------------------
// SampledList tests
// ---------------------------------------------------------------------------

static void test_list_push_remove()
{
  std::cout << "test_list_push_remove\n";

  SampledList list;
  SampledAlloc a, b, c;
  a.ptr = &a; b.ptr = &b; c.ptr = &c;

  list.push(&a);
  list.push(&b);
  list.push(&c);

  size_t count = 0;
  list.iterate([&](const SampledAlloc&) { count++; });
  assert(count == 3);

  list.remove(&b);
  count = 0;
  list.iterate([&](const SampledAlloc&) { count++; });
  assert(count == 2);

  list.remove(&a);
  list.remove(&c);
  count = 0;
  list.iterate([&](const SampledAlloc&) { count++; });
  assert(count == 0);
}

static void test_list_remove_head()
{
  std::cout << "test_list_remove_head\n";

  SampledList list;
  SampledAlloc a, b;

  list.push(&a);
  list.push(&b); // b becomes head (LIFO)
  list.remove(&b);

  size_t count = 0;
  void* found = nullptr;
  list.iterate([&](const SampledAlloc& n) {
    count++;
    found = n.ptr;
  });
  assert(count == 1);
  assert(found == a.ptr);
}

// Multiple threads push concurrently; all nodes must appear in iteration.
static void test_list_concurrent_push()
{
  std::cout << "test_list_concurrent_push\n";

  SampledList list;
  const size_t n_threads = 8;
  const size_t n_per_thread = 128;

  std::vector<std::vector<SampledAlloc>> nodes(n_threads,
    std::vector<SampledAlloc>(n_per_thread));
  std::vector<std::thread> threads;

  for (size_t t = 0; t < n_threads; t++)
  {
    threads.emplace_back([&, t]() {
      for (size_t i = 0; i < n_per_thread; i++)
      {
        nodes[t][i].ptr = &nodes[t][i];
        list.push(&nodes[t][i]);
      }
    });
  }
  for (auto& th : threads)
    th.join();

  size_t count = 0;
  list.iterate([&](const SampledAlloc&) { count++; });

  if (count != n_threads * n_per_thread)
  {
    std::cerr << "concurrent push: expected " << n_threads * n_per_thread
              << " nodes, got " << count << "\n";
    abort();
  }
}

// ---------------------------------------------------------------------------

int main()
{
  test_sampler_rate();
  test_sampler_disabled();
  test_sampler_large_alloc();
  test_list_push_remove();
  test_list_remove_head();
  test_list_concurrent_push();

  std::cout << "all sampler tests passed\n";
  return 0;
}
