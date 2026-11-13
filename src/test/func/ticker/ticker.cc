#include <snmalloc/mem/ticker.h>
#include <test/setup.h>

using namespace snmalloc;

namespace
{
  struct TestPal
  {
    static constexpr uint64_t pal_features = PalFeatures::Time;
    inline static uint64_t now = 1;

    static uint64_t time_in_ms()
    {
      return now;
    }
  };

  void test_sample_callback_and_long_gap()
  {
    Ticker<TestPal> ticker;
    size_t callbacks = 0;
    uint64_t last_sample = 0;

    ticker.check_tick(nullptr, [&](uint64_t sample) {
      callbacks++;
      last_sample = sample;
    });
    SNMALLOC_CHECK(callbacks == 1);
    SNMALLOC_CHECK(last_sample == 1);

    TestPal::now = 1001;
    ticker.check_tick(nullptr, [&](uint64_t sample) {
      callbacks++;
      last_sample = sample;
    });
    SNMALLOC_CHECK(callbacks == 2);
    SNMALLOC_CHECK(last_sample == 1001);

    TestPal::now = 1002;
    ticker.check_tick(nullptr, [&](uint64_t sample) {
      callbacks++;
      last_sample = sample;
    });
    SNMALLOC_CHECK(callbacks == 3);
    SNMALLOC_CHECK(last_sample == 1002);
  }

  void test_zero_duration_callback()
  {
    Ticker<TestPal> ticker;
    size_t callbacks = 0;
    TestPal::now = 10;

    ticker.check_tick(nullptr, [&](uint64_t) { callbacks++; });
    ticker.check_tick(nullptr, [&](uint64_t) { callbacks++; });
    SNMALLOC_CHECK(callbacks == 2);

    ticker.check_tick(nullptr, [&](uint64_t) { callbacks++; });
    SNMALLOC_CHECK(callbacks == 3);
    ticker.check_tick(nullptr, [&](uint64_t) { callbacks++; });
    SNMALLOC_CHECK(callbacks == 3);
  }
}

int main()
{
  setup();
  test_sample_callback_and_long_gap();
  test_zero_duration_callback();
}
