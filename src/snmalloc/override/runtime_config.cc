// SPDX-License-Identifier: MIT
//
// C ABI shims for the Phase 9.7 runtime tunables.  The
// implementation is intentionally tiny -- each function is a
// one-line passthrough to the `snmalloc::RuntimeConfig` singleton in
// `src/snmalloc/global/runtime_config.h`.  Symbols are exported
// unconditionally (independent of the `SNMALLOC_PROFILE` /
// `SNMALLOC_STATS` flags) because runtime tunables are useful in
// every build configuration -- the sampling-rate knob remains a
// no-op when the profiler is compiled out, but the decay-rate and
// local-cache caps are independent of profiling.
//
// The sample-interval setter additionally mirrors the value into
// `snmalloc::profile::Sampler::set_sampling_rate` so the profiler's
// existing global picks it up without any consumer in profile/* having
// to learn about `RuntimeConfig`.  This keeps the sampler hot-path
// unchanged: it still reads its own `SamplerGlobals::sampling_rate()`
// atomic on the slow path, just now seeded from `RuntimeConfig` at
// every set point.
//
// All getters are safe to call from any thread at any point in the
// process lifetime, including before the first allocation; see the
// `RuntimeConfig` header for the lazy-init contract.

#include "snmalloc/global/runtime_config.h"

#include "../snmalloc.h"

#ifdef SNMALLOC_PROFILE
#  include "../profile/sampler.h"
#endif

#include <stdint.h>

#ifndef SNMALLOC_EXPORT
#  define SNMALLOC_EXPORT
#endif

using snmalloc::RuntimeConfig;

extern "C" SNMALLOC_EXPORT void snmalloc_set_sample_interval(uint64_t bytes)
{
  RuntimeConfig::set_sample_interval_bytes(bytes);
#ifdef SNMALLOC_PROFILE
  // Mirror into the profiler's globals so existing slow-path readers
  // (which only consult `SamplerGlobals::sampling_rate()`) observe the
  // new value without needing to learn about `RuntimeConfig`.  In
  // non-profile builds the sampler is compiled out entirely; the
  // tunable still round-trips through `RuntimeConfig` so callers can
  // pre-seed a value that takes effect when the binary is rebuilt
  // with profiling on.
  snmalloc::profile::Sampler::set_sampling_rate(static_cast<size_t>(bytes));
#endif
}

extern "C" SNMALLOC_EXPORT void snmalloc_set_decay_rate(uint32_t milliseconds)
{
  RuntimeConfig::set_decay_rate_ms(milliseconds);
}

extern "C" SNMALLOC_EXPORT void snmalloc_set_max_local_cache(uint64_t bytes)
{
  RuntimeConfig::set_max_local_cache_bytes(bytes);
}

extern "C" SNMALLOC_EXPORT uint64_t snmalloc_get_sample_interval(void)
{
  return RuntimeConfig::sample_interval_bytes();
}

extern "C" SNMALLOC_EXPORT uint32_t snmalloc_get_decay_rate(void)
{
  return RuntimeConfig::decay_rate_ms();
}

extern "C" SNMALLOC_EXPORT uint64_t snmalloc_get_max_local_cache(void)
{
  return RuntimeConfig::max_local_cache_bytes();
}
