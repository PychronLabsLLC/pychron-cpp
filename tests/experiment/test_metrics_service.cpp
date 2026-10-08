// MetricsService: what the application builds from `[metrics]`.
#include "pychron/experiment/metrics/service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "http_client.hpp"
#include "metrics_text.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/config/metrics_config.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/metrics/server.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using experiment::metrics::MetricsService;
using metrics_text::has;
using metrics_text::value;

namespace {

struct MetricsServiceTest : ::testing::Test {
  ManualClock clock;
  SignalBus bus;
  Scheduler scheduler{clock, &bus, Scheduler::Options{0}};

  std::mutex m;
  std::vector<Alarm> alarms;
  std::vector<Log> logs;
  SignalBus::Subscription s1 = bus.subscribe<Alarm>([this](const Alarm& a) {
    std::lock_guard lock(m);
    alarms.push_back(a);
  });
  SignalBus::Subscription s2 = bus.subscribe<Log>([this](const Log& l) {
    std::lock_guard lock(m);
    logs.push_back(l);
  });

  // On the loopback interface, at a port the OS picks.
  static config::MetricsConfig enabled() {
    config::MetricsConfig c;
    c.enabled = true;
    c.bind = "127.0.0.1";
    c.port = 0;
    return c;
  }

  std::unique_ptr<MetricsService> start(const config::MetricsConfig& c) {
    return MetricsService::start(c, bus, scheduler, clock, nullptr, "9.9.9");
  }

  bool logged(LogLevel level, const std::string& part) {
    std::lock_guard lock(m);
    for (const Log& l : logs) {
      if (l.level == level && l.logger == "metrics" && l.message.find(part) != std::string::npos) return true;
    }
    return false;
  }
};

}  // namespace

TEST_F(MetricsServiceTest, DisabledBuildsNothing) {
  config::MetricsConfig c = enabled();
  c.enabled = false;
  const std::size_t jobs = scheduler.job_count();
  EXPECT_EQ(start(c), nullptr);
  EXPECT_EQ(scheduler.job_count(), jobs);
  EXPECT_TRUE(alarms.empty());
}

TEST_F(MetricsServiceTest, EnabledServesWhatTheBusSays) {
  auto service = start(enabled());
  ASSERT_NE(service, nullptr);
  ASSERT_TRUE(service->listening());
  ASSERT_NE(service->port(), 0);
  bus.publish(PressureSample{"IG1", 3e-9, "torr", {}});

  const std::string body = http_client::body(http_client::get(service->port(), "/metrics"));
  EXPECT_DOUBLE_EQ(value(body, "pychron_pressure{gauge=\"IG1\",unit=\"torr\"}"), 3e-9);
  EXPECT_NE(body.find("pychron_build_info{"), std::string::npos);
  EXPECT_NE(body.find("version=\"9.9.9\""), std::string::npos);
  // Each of the exporters is there: the experiment's, the scheduler's, the server's own.
  EXPECT_DOUBLE_EQ(value(body, "pychron_executor_state{state=\"idle\"}"), 1.0);
  EXPECT_TRUE(has(body, "pychron_scheduler_heartbeat_age_seconds"));
  EXPECT_TRUE(has(body, "pychron_metrics_bad_requests_total"));
  EXPECT_TRUE(alarms.empty());
}

TEST_F(MetricsServiceTest, SaysWhereItListens) {
  auto service = start(enabled());
  ASSERT_NE(service, nullptr);
  EXPECT_TRUE(logged(LogLevel::Info, "listening on 127.0.0.1:" + std::to_string(service->port())));
}

TEST_F(MetricsServiceTest, APortInUseLeavesTheApplicationRunning) {
  pychron::metrics::Registry other;
  pychron::metrics::MetricsServer::Options o;
  o.bind = "127.0.0.1";
  o.port = 0;
  auto holder = pychron::metrics::MetricsServer::start(other, o);
  ASSERT_TRUE(holder.has_value());

  config::MetricsConfig c = enabled();
  c.port = (*holder)->port();
  auto service = start(c);
  ASSERT_NE(service, nullptr);
  EXPECT_FALSE(service->listening());
  EXPECT_EQ(service->port(), 0);
  {
    std::lock_guard lock(m);
    ASSERT_EQ(alarms.size(), 1u);
    EXPECT_EQ(alarms[0].source, "metrics");
    EXPECT_EQ(alarms[0].severity, AlarmSeverity::Warning);
    EXPECT_NE(alarms[0].message.find("metrics endpoint is off"), std::string::npos) << alarms[0].message;
    EXPECT_NE(alarms[0].message.find(std::to_string(c.port)), std::string::npos) << alarms[0].message;
  }
  EXPECT_TRUE(logged(LogLevel::Error, "metrics endpoint is off"));
  // The rest of the application is untouched, and the registry still follows the bus.
  bus.publish(PressureSample{"IG1", 1e-9, "torr", {}});
  EXPECT_DOUBLE_EQ(value(service->registry().render(), "pychron_pressure{gauge=\"IG1\",unit=\"torr\"}"), 1e-9);
}

TEST_F(MetricsServiceTest, ABindThatIsNotThisMachinesIsAnAlarmNotACrash) {
  config::MetricsConfig c = enabled();
  c.bind = "203.0.113.7";  // TEST-NET-3: no interface here has it
  auto service = start(c);
  ASSERT_NE(service, nullptr);
  EXPECT_FALSE(service->listening());
  std::lock_guard lock(m);
  EXPECT_EQ(alarms.size(), 1u);
}

TEST_F(MetricsServiceTest, AFullFamilyIsLoggedOnce) {
  auto service = start(enabled());
  ASSERT_NE(service, nullptr);
  for (std::size_t i = 0; i < pychron::metrics::Registry::kMaxSeriesPerFamily + 3; ++i) {
    bus.publish(PressureSample{"IG" + std::to_string(i), 1e-9, "torr", {}});
  }
  std::lock_guard lock(m);
  int said = 0;
  for (const Log& l : logs) {
    if (l.level == LogLevel::Warn && l.message.find("pychron_pressure") != std::string::npos) ++said;
  }
  EXPECT_EQ(said, 1);
}

TEST_F(MetricsServiceTest, DestructionRemovesTheHeartbeatJobAndFreesThePort) {
  const std::size_t jobs = scheduler.job_count();
  config::MetricsConfig c = enabled();
  {
    auto service = start(c);
    ASSERT_NE(service, nullptr);
    c.port = service->port();
    EXPECT_EQ(scheduler.job_count(), jobs + 1);
  }
  EXPECT_EQ(scheduler.job_count(), jobs);
  // The same port can be had again at once, as after a restart of the application.
  auto again = start(c);
  ASSERT_NE(again, nullptr);
  EXPECT_TRUE(again->listening());
  std::lock_guard lock(m);
  EXPECT_TRUE(alarms.empty());
}
