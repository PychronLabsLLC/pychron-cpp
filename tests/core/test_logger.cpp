#include <gtest/gtest.h>

#include <sstream>
#include <thread>
#include <vector>

#include "pychron/core/log_context.hpp"
#include "pychron/core/logger.hpp"
#include "pychron/core/signal_bus.hpp"

using namespace pychron;
using namespace std::chrono_literals;

TEST(Logger, PublishesLogEventsAtOrAboveLevel) {
  ManualClock clock;
  clock.advance(5s);
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });

  Logger log("switches", clock, &bus, {LogLevel::Info, nullptr});
  log.debug("hidden");
  log.info("valve A opened");
  log.error("valve B failed");

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].level, LogLevel::Info);
  EXPECT_EQ(got[0].logger, "switches");
  EXPECT_EQ(got[0].message, "valve A opened");
  EXPECT_EQ(got[0].ts, clock.now());
  EXPECT_EQ(got[1].level, LogLevel::Error);
}

TEST(Logger, LegacyEventCarriesThreadAndRun) {
  ManualClock clock;
  SignalBus bus;
  std::vector<Log> got;
  auto sub = bus.subscribe<Log>([&](const Log& e) { got.push_back(e); });
  Logger log("switches", clock, &bus, {LogLevel::Info, nullptr});

  {
    const LogContext::Thread t{"executor"};
    const LogContext::Run r{"12345-01A"};
    log.info("in a run");
  }
  std::thread worker([&log] { log.info("untagged"); });
  worker.join();

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0].thread, "executor");
  EXPECT_EQ(got[0].run, "12345-01A");
  ASSERT_GE(got[1].thread.size(), 2u);
  EXPECT_EQ(got[1].thread.front(), 't');
  EXPECT_EQ(got[1].thread.find_first_not_of("0123456789", 1), std::string::npos) << got[1].thread;
  EXPECT_EQ(got[1].run, "");
}

TEST(Logger, EchoesToStreamAndSupportsLevelChanges) {
  ManualClock clock;
  std::ostringstream out;
  Logger log("core", clock, nullptr, {LogLevel::Warn, &out});
  log.info("skip");
  log.warn("careful");
  log.set_level(LogLevel::Trace);
  EXPECT_TRUE(log.enabled(LogLevel::Trace));
  log.trace("detail");
  EXPECT_EQ(out.str(), "[warn] core: careful\n[trace] core: detail\n");
}

TEST(Logger, ChildInheritsSettings) {
  ManualClock clock;
  SignalBus bus;
  std::vector<std::string> names;
  auto sub = bus.subscribe<Log>([&](const Log& e) { names.push_back(e.logger); });
  Logger parent("systems", clock, &bus, {LogLevel::Debug, nullptr});
  Logger child = parent.child("gauges");
  EXPECT_EQ(child.name(), "systems.gauges");
  EXPECT_EQ(child.level(), LogLevel::Debug);
  child.debug("x");
  EXPECT_EQ(names, (std::vector<std::string>{"systems.gauges"}));
}

TEST(Logger, LevelNames) {
  EXPECT_EQ(to_string(LogLevel::Trace), "trace");
  EXPECT_EQ(to_string(LogLevel::Debug), "debug");
  EXPECT_EQ(to_string(LogLevel::Info), "info");
  EXPECT_EQ(to_string(LogLevel::Warn), "warn");
  EXPECT_EQ(to_string(LogLevel::Error), "error");
}
