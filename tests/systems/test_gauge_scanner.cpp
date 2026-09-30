#include <gtest/gtest.h>

#include <vector>

#include "pychron/systems/gauge_scanner.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {
struct Fixture {
  ManualClock clock;
  SignalBus bus;
  Scheduler sched{clock, &bus, Scheduler::Options{0}};
  GaugeScanner scanner{sched, bus, clock};
  std::vector<PressureSample> samples;
  std::vector<Alarm> alarms;
  SignalBus::Subscription s1 = bus.subscribe<PressureSample>([this](const PressureSample& e) { samples.push_back(e); });
  SignalBus::Subscription s2 = bus.subscribe<Alarm>([this](const Alarm& e) { alarms.push_back(e); });

  void tick() {
    clock.advance(100ms);
    sched.run_pending();
  }
};

config::GaugeConfig cfg() {
  config::GaugeConfig g;
  g.name = "IG1";
  g.units = config::PressureUnits::Torr;
  g.alarm_high = 1e-4;
  g.alarm_low = 1e-9;
  return g;
}

struct FakeGauge : IPressureGauge {
  Result<double> next = 1e-6;
  Result<double> read_pressure() override { return next; }
};

struct FakeCtl : IChannelPressureGauge {
  int last = 0;
  std::vector<int> pressure_channels() const override { return {1, 2}; }
  Result<double> read_pressure(int ch) override { last = ch; return 1e-6; }
};
}  // namespace

TEST(GaugeScanner, PublishesSampleEachInterval) {
  Fixture f;
  FakeGauge g;
  ASSERT_TRUE(f.scanner.add_gauge(cfg(), g, 100ms));
  f.tick();
  f.tick();
  ASSERT_EQ(f.samples.size(), 2u);
  EXPECT_EQ(f.samples[0].gauge, "IG1");
  EXPECT_EQ(f.samples[0].units, "torr");
  EXPECT_DOUBLE_EQ(f.samples[0].value, 1e-6);
  EXPECT_TRUE(f.alarms.empty());
}

TEST(GaugeScanner, HighAlarmOnEdgeAndRearms) {
  Fixture f;
  FakeGauge g;
  ASSERT_TRUE(f.scanner.add_gauge(cfg(), g, 100ms));
  g.next = 1e-3;
  f.tick();
  f.tick();
  ASSERT_EQ(f.alarms.size(), 1u);  // no repeat while still high
  EXPECT_EQ(f.alarms[0].source, "IG1");
  EXPECT_EQ(f.alarms[0].severity, AlarmSeverity::Critical);
  g.next = 1e-6;
  f.tick();
  g.next = 1e-3;
  f.tick();
  EXPECT_EQ(f.alarms.size(), 2u);
}

TEST(GaugeScanner, LowAlarm) {
  Fixture f;
  FakeGauge g;
  g.next = 1e-12;
  ASSERT_TRUE(f.scanner.add_gauge(cfg(), g, 100ms));
  f.tick();
  ASSERT_EQ(f.alarms.size(), 1u);
  EXPECT_NE(f.alarms[0].message.find("alarm_low"), std::string::npos);
}

TEST(GaugeScanner, NoThresholdsNoAlarm) {
  Fixture f;
  FakeGauge g;
  g.next = 1e3;
  config::GaugeConfig c = cfg();
  c.alarm_high.reset();
  c.alarm_low.reset();
  ASSERT_TRUE(f.scanner.add_gauge(c, g, 100ms));
  f.tick();
  EXPECT_TRUE(f.alarms.empty());
  EXPECT_EQ(f.samples.size(), 1u);
}

TEST(GaugeScanner, ReadFailureAlarmsOnceNoSample) {
  Fixture f;
  FakeGauge g;
  g.next = fail(ErrorKind::Timeout, "no reply", "ig");
  ASSERT_TRUE(f.scanner.add_gauge(cfg(), g, 100ms));
  f.tick();
  f.tick();
  EXPECT_TRUE(f.samples.empty());
  ASSERT_EQ(f.alarms.size(), 1u);
  EXPECT_EQ(f.alarms[0].severity, AlarmSeverity::Warning);
}

TEST(GaugeScanner, ChannelGaugeUsesConfiguredChannel) {
  Fixture f;
  FakeCtl c;
  config::GaugeConfig g = cfg();
  g.channel = 2;
  ASSERT_TRUE(f.scanner.add_gauge(g, c, 100ms));
  f.tick();
  EXPECT_EQ(c.last, 2);
  EXPECT_EQ(f.samples.size(), 1u);
}

TEST(GaugeScanner, StopCancelsJobs) {
  Fixture f;
  FakeGauge g;
  ASSERT_TRUE(f.scanner.add_gauge(cfg(), g, 100ms));
  f.scanner.stop();
  f.tick();
  EXPECT_TRUE(f.samples.empty());
}

TEST(GaugeScanner, BadIntervalIsError) {
  Fixture f;
  FakeGauge g;
  auto r = f.scanner.add_gauge(cfg(), g, 0ms);
  EXPECT_FALSE(r);
  EXPECT_EQ(f.scanner.gauge_count(), 0u);
}
