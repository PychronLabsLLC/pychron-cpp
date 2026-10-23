#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "pychron/core/log_error.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/process_log.hpp"
#include "log_capture.hpp"
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

TEST(LogAndPublish, WithoutAProcessHubTheLineGoesToTheBusAlone) {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  log_and_publish(process_logger("src"), &bus, "src", LogLevel::Warn, "said", clock.now());
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].logger, "src");
  EXPECT_EQ(got[0].message, "said");
}

TEST(LogAndPublish, WithAProcessHubTheLineIsInTheFileAndOnAnotherBusOnce) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  log_and_publish(process_logger("src"), &bus, "src", LogLevel::Warn, "said", clock.now());
  EXPECT_EQ(got.size(), 1u);
  const auto lines = log.lines();
  EXPECT_NE(std::find(lines.begin(), lines.end(), "[warn] src: said"), lines.end());
}

TEST(LogAndPublish, OnTheHubsOwnBusTheLineIsPublishedOnce) {
  pychron::testing::LogCapture log;
  ASSERT_TRUE(log.ok());
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  const auto attached = log.hub().attach_bus(bus);
  log_and_publish(process_logger("src"), &bus, "src", LogLevel::Warn, "said", clock.now());
  EXPECT_EQ(got.size(), 1u);  // by the hub, not again by the helper
}
