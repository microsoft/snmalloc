#pragma once
#ifdef _MSC_VER
#  define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

namespace snmalloc
{
  /**
   * The name of the function under test.  This is set in the START_TEST macro
   * and used for error reporting in EXPECT.
   */
  const char* current_test = "";

  /**
   * Log that the test started.
   */
#define START_TEST(msg, ...) \
  do \
  { \
    current_test = __PRETTY_FUNCTION__; \
    MessageBuilder<1024> mb{"Starting test: " msg "\n", ##__VA_ARGS__}; \
    DefaultPal::message(mb.get_message()); \
  } while (0)

  /**
   * An assertion that fires even in debug builds.  Uses the value set by
   * START_TEST.
   */
#define EXPECT(x, msg, ...) \
  SNMALLOC_CHECK_MSG(x, " in test {} " msg "\n", current_test, ##__VA_ARGS__)

#define INFO(msg, ...) \
  do \
  { \
    MessageBuilder<1024> mb{msg "\n", ##__VA_ARGS__}; \
    DefaultPal::message(mb.get_message()); \
  } while (0)

}
