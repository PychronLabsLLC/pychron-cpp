#include <gtest/gtest.h>

#include <filesystem>

#include "pychron/core/process_log.hpp"

namespace pychron {

TEST(ProcessLog, ProcessLoggerWithoutHubWritesNothing) {
  const ProcessLogScope none(nullptr);
  EXPECT_EQ(process_log_hub(), nullptr);
  const Logger log = process_logger("x");
  EXPECT_EQ(log.name(), "x");
  EXPECT_FALSE(log.enabled(LogLevel::Error));
  log.error("nothing");  // must not crash
}

TEST(ProcessLog, ScopeRestoresPrevious) {
  SteadyClock clock;
  config::LoggingConfig cfg;
  auto a = LogHub::create(cfg, clock);
  auto b = LogHub::create(cfg, clock);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  const ProcessLogScope outer(*a);
  EXPECT_EQ(process_log_hub(), *a);
  {
    const ProcessLogScope inner(*b);
    EXPECT_EQ(process_log_hub(), *b);
    EXPECT_TRUE(process_logger("y").enabled(LogLevel::Error));
  }
  EXPECT_EQ(process_log_hub(), *a);
}

}  // namespace pychron
