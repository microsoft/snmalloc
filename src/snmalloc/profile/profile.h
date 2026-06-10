// SPDX-License-Identifier: MIT
//
// Heap profiler -- umbrella header for the snmalloc heap-profile subsystem.
//
// Phase 2.2 of the heap-profiling milestone. Purely additive; including
// this header does NOT enable profiling on any allocator path. The
// integration with snmalloc::alloc()/dealloc() is Phase 3 work.
//
// Components:
//   sampler.h           -- per-thread Poisson sampler
//   sampled_alloc.h     -- one record per sampled allocation
//   node_pool.h         -- pre-allocated lock-free pool of records
//   sampled_list.h      -- lock-free intrusive list of live samples
//   reentrancy_guard.h  -- per-thread guard against sampler recursion

#pragma once

#include "node_pool.h"
#include "reentrancy_guard.h"
#include "sampled_alloc.h"
#include "sampled_list.h"
#include "sampler.h"
