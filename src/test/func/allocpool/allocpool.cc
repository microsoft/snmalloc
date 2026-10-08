#include <snmalloc/backend/fixedglobalconfig.h>
#include <snmalloc/snmalloc.h>
#include <test/setup.h>
#include <thread>

using namespace snmalloc;

namespace
{
  constexpr uint64_t idle_constant_ms = uint64_t{SNMALLOC_ASSIST_IDLE_MS};
  static_assert(idle_constant_ms >= 2);

  struct AlternateConfig
  {
    using Pal = DefaultPal;

    static constexpr Flags Options = []() constexpr {
      Flags opts = {};
      opts.IsQueueInline = false;
      opts.QueueHeadsAreTame = false;
      opts.AllocOwnsLocalState = false;
      return opts;
    }();
  };

  struct NonPooledConfig
  {
    using Pal = DefaultPal;

    static constexpr Flags Options = []() constexpr {
      Flags opts = {};
      opts.AllocIsPoolAllocated = false;
      return opts;
    }();
  };

  struct SchedulingConfig
  {
    using Pal = DefaultPal;

    static constexpr Flags Options = []() constexpr {
      Flags opts = {};
      opts.AllocIsPoolAllocated = true;
      return opts;
    }();
  };

  static_assert(uses_inactive_queue_marker<snmalloc::Config>);
  static_assert(uses_inactive_queue_marker<FixedRangeConfig<DefaultPal>>);
  static_assert(
    !uses_inactive_queue_marker<FixedRangeConfig<PALNoAlloc<DefaultPal>>>);
  static_assert(!uses_inactive_queue_marker<NonPooledConfig>);
  static_assert(uses_inactive_queue_marker<AlternateConfig>);

  template<typename Config>
  Allocator<Config>* last_in_chain(Allocator<Config>* first)
  {
    auto* last = first;
    while (last != nullptr)
    {
      auto* next = AllocPool<Config>::extract(last);
      if (next == nullptr)
        return last;
      last = next;
    }
    return nullptr;
  }

  void test_reservation_and_pacing()
  {
    using Assistance = AllocPoolAssistance<SchedulingConfig>;

    SNMALLOC_CHECK(Assistance::debug_pending_count() == 0);
    SNMALLOC_CHECK(!Assistance::should_assist(1));

    Assistance::pending_queue_claimed();
    SNMALLOC_CHECK(Assistance::debug_pending_count() == -1);
    SNMALLOC_CHECK(!Assistance::should_assist(1));
    Assistance::pending_queue_added();
    SNMALLOC_CHECK(!Assistance::should_assist(1 + idle_constant_ms));

    Assistance::pending_queue_added();
    SNMALLOC_CHECK(Assistance::should_assist(1 + idle_constant_ms));
    SNMALLOC_CHECK(!Assistance::should_assist(1 + idle_constant_ms));
    SNMALLOC_CHECK(Assistance::should_assist(idle_constant_ms));
    SNMALLOC_CHECK(!Assistance::should_assist(1 + idle_constant_ms));

    Assistance::pending_queue_claimed();
    Assistance::pending_queue_added();
    std::atomic<bool> start{false};
    std::atomic<size_t> assisted{0};
    auto assist_after_start = [&start]() {
      while (!start.load(std::memory_order_acquire))
      {}
      SNMALLOC_CHECK(!Assistance::should_assist(2 * idle_constant_ms));
    };
    std::thread first(assist_after_start);
    std::thread second(assist_after_start);
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    start.store(false, std::memory_order_relaxed);
    auto reserve_after_start = [&start, &assisted]() {
      while (!start.load(std::memory_order_acquire))
      {}
      if (Assistance::should_assist(3 * idle_constant_ms))
        assisted.fetch_add(1, std::memory_order_relaxed);
    };
    std::thread third(reserve_after_start);
    std::thread fourth(reserve_after_start);
    start.store(true, std::memory_order_release);
    third.join();
    fourth.join();
    SNMALLOC_CHECK(assisted.load(std::memory_order_relaxed) == 1);

    Assistance::pending_queue_claimed();
    SNMALLOC_CHECK(Assistance::debug_pending_count() == 0);
  }

  void add_forwarding_work()
  {
    using Pool = AllocPool<snmalloc::Config>;

    auto* owner = Pool::acquire();
    auto* sender = Pool::acquire();
    void* p = owner->alloc(64);

    owner->flush();
    Pool::release(owner);
    sender->dealloc(p);
    Pool::release(sender);
  }

  void test_single_pass_cleanup()
  {
    using TestConfig = snmalloc::Config;
    using Pool = AllocPool<TestConfig>;
    using Assistance = AllocPoolAssistance<TestConfig>;

    auto* saved = Pool::extract();
    auto* saved_last = last_in_chain<TestConfig>(saved);

    add_forwarding_work();
    SNMALLOC_CHECK(Assistance::debug_pending_count() == 0);

    cleanup_unused<TestConfig>();
    SNMALLOC_CHECK(Assistance::debug_pending_count() == 1);

    cleanup_unused<TestConfig>();
    SNMALLOC_CHECK(Assistance::debug_pending_count() == 0);

    auto* assisting = Pool::try_acquire_front();
    SNMALLOC_CHECK(assisting != nullptr);
    SNMALLOC_CHECK(assisting->debug_is_in_use());
    cleanup_unused<TestConfig>();
    SNMALLOC_CHECK(assisting->debug_is_in_use());
    Pool::release(assisting);

    bool empty = false;
    debug_check_empty<TestConfig>(&empty);
    SNMALLOC_CHECK(empty);

    if (saved != nullptr)
      Pool::restore(saved, saved_last);
  }
}

int main()
{
  setup();
  test_reservation_and_pacing();
  test_single_pass_cleanup();
}
