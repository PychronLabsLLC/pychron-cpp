#include <gtest/gtest.h>

#include <vector>

#include "pychron/core/log_error.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/signal_bus.hpp"

using namespace pychron;

namespace {
struct Capture {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  SignalBus::Subscription sub = bus.subscribe<Log>([this](const Log& e) { got.push_back(e); });
  Logger logger{"test", clock, &bus, {LogLevel::Trace, nullptr}};
};
}  // namespace

TEST(LogError, FormatWithContext) {
  Capture c;
  log_error(c.logger, Error{ErrorKind::Io, "timeout", "link ngx"}, "open valve A");
  ASSERT_EQ(c.got.size(), 1u);
  EXPECT_EQ(c.got[0].level, LogLevel::Error);
  EXPECT_EQ(c.got[0].message, "open valve A: io: timeout (link ngx)");
}

TEST(LogError, FormatWithoutContext) {
  Capture c;
  log_error(c.logger, Error{ErrorKind::NotConnected, "no link", ""}, "read pressure");
  ASSERT_EQ(c.got.size(), 1u);
  EXPECT_EQ(c.got[0].message, "read pressure: not_connected: no link");
}

TEST(LogError, HandledIsWarn) {
  Capture c;
  log_handled(c.logger, Error{ErrorKind::Timeout, "slow", ""}, "retry");
  ASSERT_EQ(c.got.size(), 1u);
  EXPECT_EQ(c.got[0].level, LogLevel::Warn);
  EXPECT_EQ(c.got[0].message, "retry: timeout: slow");
}
