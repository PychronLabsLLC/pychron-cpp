// The line's heaters: [[heaters]], the scan and the commands (plan
// 2026-10-05, task E1), on simulated PLC heaters.
#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <vector>

#include "pychron/core/config/loader.hpp"
#include "pychron/devices/extraction/capability.hpp"
#include "pychron/systems/extraction_line.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
using namespace std::chrono_literals;

constexpr const char* kSystem = R"(
[system]
name = "t"
scan_interval_ms = 1000
[transports.plc]
kind = "sim"
timeout_ms = 50
[transports.plc2]
kind = "sim"
timeout_ms = 50
[drivers.furnace_plc]
kind = "plc2000_heater"
transport = "plc"
enable = 2
use_pid = 1
setpoint = 11
readback = 21
[drivers.bake_plc]
kind = "plc2000_heater"
transport = "plc2"
enable = 1
readback = 3
[[heaters]]
name = "furnace"
driver = "furnace_plc"
description = "Furnace heater"
units = "C"
[[heaters]]
name = "bake"
driver = "bake_plc"
)";

struct Line {
  explicit Line(const char* toml = kSystem) {
    auto cfg = config::load_system_config_from_string(toml, "t.toml");
    EXPECT_TRUE(cfg) << cfg.error().what;
    ExtractionLine::Options o;
    o.clock = &clock;
    o.scheduler.threads = 0;
    o.run_scheduler = false;
    auto made = ExtractionLine::create(*cfg, std::nullopt, o);
    EXPECT_TRUE(made) << made.error().what;
    line = std::move(*made);
    subs.push_back(line->bus().subscribe<HeaterSample>([this](const HeaterSample& s) {
      std::lock_guard lock(m);
      samples.push_back(s);
    }));
    subs.push_back(line->bus().subscribe<Alarm>([this](const Alarm& a) {
      std::lock_guard lock(m);
      alarms.push_back(a);
    }));
  }
  ~Line() { line->stop(); }

  std::vector<HeaterSample> taken() {
    std::lock_guard lock(m);
    return samples;
  }
  std::vector<Alarm> raised() {
    std::lock_guard lock(m);
    return alarms;
  }
  void scan() {
    clock.advance(1s);
    line->scheduler().run_pending();
  }

  ManualClock clock;
  std::unique_ptr<ExtractionLine> line;
  std::mutex m;
  std::vector<HeaterSample> samples;
  std::vector<Alarm> alarms;
  std::vector<SignalBus::Subscription> subs;
};

}  // namespace

TEST(HeaterConfig, LoadsAndChecks) {
  auto cfg = config::load_system_config_from_string(kSystem, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ASSERT_EQ(cfg->heaters.size(), 2u);
  EXPECT_EQ(cfg->heaters[0].name, "furnace");
  EXPECT_EQ(cfg->heaters[0].driver, "furnace_plc");
  EXPECT_EQ(cfg->heaters[0].description, "Furnace heater");
  EXPECT_EQ(cfg->heaters[0].units, "C");
  EXPECT_EQ(cfg->heaters[1].units, "");
  const std::string base =
      "[system]\nname = \"t\"\n[transports.p]\nkind = \"sim\"\n[drivers.h]\nkind = \"plc2000_heater\"\ntransport = \"p\"\n";
  EXPECT_TRUE(config::load_system_config_from_string(base + "[[heaters]]\nname = \"a\"\ndriver = \"h\"\n", "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(base + "[[heaters]]\nname = \"a\"\ndriver = \"nope\"\n", "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(base + "[[heaters]]\ndriver = \"h\"\n", "t.toml"));
  EXPECT_FALSE(
      config::load_system_config_from_string(base + "[[heaters]]\nname = \"a\"\ndriver = \"h\"\nstray = 1\n", "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(
      base + "[[heaters]]\nname = \"a\"\ndriver = \"h\"\n[[heaters]]\nname = \"a\"\ndriver = \"h\"\n", "t.toml"));
}

TEST(LineHeaters, StartScansEveryFieldOfEveryHeater) {
  Line l;
  EXPECT_FALSE(l.line->heater_info("furnace"));
  ASSERT_TRUE(l.line->start());
  auto samples = l.taken();
  ASSERT_EQ(samples.size(), 2u);
  const auto& f = samples[0];
  EXPECT_EQ(f.heater, "furnace");
  ASSERT_TRUE(f.readback && f.setpoint && f.enabled && f.use_pid);
  EXPECT_NEAR(*f.readback, 25.0, 1e-4);
  EXPECT_EQ(*f.enabled, false);
  // The bake heater has no setpoint or use_pid: those are not there, not errors.
  const auto& b = samples[1];
  EXPECT_EQ(b.heater, "bake");
  EXPECT_TRUE(b.readback && b.enabled);
  EXPECT_FALSE(b.setpoint);
  EXPECT_FALSE(b.use_pid);
  EXPECT_TRUE(l.raised().empty());
  ASSERT_TRUE(l.line->heater_info("bake"));
  l.scan();
  EXPECT_EQ(l.taken().size(), 4u);
}

TEST(LineHeaters, AnEnableFromThePlcsOwnPanelIsSeen) {
  Line l;
  ASSERT_TRUE(l.line->start());
  // Someone switches the furnace on at the PLC; legacy read only the
  // readback, and only while it thought the heater was on.
  auto* plc = l.line->sim()->heater("furnace_plc");
  ASSERT_NE(plc, nullptr);
  ASSERT_TRUE(l.line->heater("furnace")->set_enabled(true));
  l.scan();
  EXPECT_EQ(*l.line->heater_info("furnace")->enabled, true);
  EXPECT_TRUE(plc->enabled());
}

TEST(LineHeaters, CommandsWriteReadBackAndPublish) {
  Line l;
  ASSERT_TRUE(l.line->start());
  const auto before = l.taken().size();
  ASSERT_TRUE(l.line->set_heater_setpoint("furnace", 450.0));
  ASSERT_TRUE(l.line->set_heater_enabled("furnace", true));
  ASSERT_TRUE(l.line->set_heater_pid("furnace", true));
  const auto samples = l.taken();
  ASSERT_EQ(samples.size(), before + 3);
  EXPECT_EQ(*samples.back().setpoint, 450.0);
  EXPECT_EQ(*samples.back().enabled, true);
  EXPECT_EQ(*samples.back().use_pid, true);
  l.clock.advance(10min);
  l.scan();
  EXPECT_NEAR(*l.line->heater_info("furnace")->readback, 450.0, 1.0);
  ASSERT_TRUE(l.line->set_heater_enabled("furnace", false));
  EXPECT_EQ(*l.line->heater_info("furnace")->enabled, false);
}

TEST(LineHeaters, ASetpointThatReadsBackDifferentlyIsAProtocolError) {
  Line l;
  ASSERT_TRUE(l.line->start());
  l.line->sim()->heater("furnace_plc")->set_setpoint_error(1.0);
  auto r = l.line->set_heater_setpoint("furnace", 450.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "furnace");
  EXPECT_NE(r.error().what.find("451"), std::string::npos) << r.error().what;
}

TEST(LineHeaters, UnsupportedAndUnknownAreConfigErrors) {
  Line l;
  ASSERT_TRUE(l.line->start());
  auto pid = l.line->set_heater_pid("bake", true);
  ASSERT_FALSE(pid);
  EXPECT_TRUE(extraction::is_not_supported(pid.error())) << pid.error().what;
  auto fraction = l.line->set_heater_setpoint("furnace", 450.5);  // int32 on the wire
  ASSERT_FALSE(fraction);
  EXPECT_EQ(fraction.error().kind, ErrorKind::Config);
  auto unknown = l.line->set_heater_enabled("nope", true);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_FALSE(l.line->read_heater("nope"));
  EXPECT_EQ(l.line->heater("nope"), nullptr);
}

TEST(LineHeaters, AFailedScanAlarmsOnceUntilTheNextGoodOne) {
  Line l;
  ASSERT_TRUE(l.line->start());
  auto* plc = l.line->sim()->heater("furnace_plc");
  plc->set_offline(true);
  l.scan();
  l.scan();
  auto alarms = l.raised();
  ASSERT_EQ(alarms.size(), 1u);
  EXPECT_EQ(alarms[0].source, "furnace");
  EXPECT_EQ(alarms[0].severity, AlarmSeverity::Warning);
  // The other heater keeps scanning.
  const auto samples = l.taken();
  EXPECT_EQ(samples.back().heater, "bake");
  plc->set_offline(false);
  l.scan();
  plc->set_offline(true);
  l.scan();
  EXPECT_EQ(l.raised().size(), 2u);
}

TEST(LineHeaters, ADriverThatIsNotAHeaterFailsStart) {
  Line l(R"(
[system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.relay]
kind = "proxr_relay"
transport = "bus"
[[heaters]]
name = "h"
driver = "relay"
)");
  auto r = l.line->start();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("not a heater"), std::string::npos);
}

TEST(LineSharedPlc, ValvesGaugesAndAHeaterOnOnePlc) {
  // AELAMS: one PLC, one Modbus TCP connection, three drivers.
  Line l(R"(
[system]
name = "t"
scan_interval_ms = 1000
[transports.plc]
kind = "sim"
timeout_ms = 50
[drivers.plc_valves]
kind = "plc2000_valves"
transport = "plc"
[drivers.plc_gauges]
kind = "plc2000_gauges"
transport = "plc"
channels = [21]
[drivers.plc_heater]
kind = "plc2000_heater"
transport = "plc"
enable = 10
setpoint = 31
readback = 33
[[valves]]
name = "A"
actuator = "plc_valves"
address = "1"
[[valves]]
name = "B"
actuator = "plc_valves"
address = "2"
[[gauges]]
name = "IG1"
driver = "plc_gauges"
channel = 21
[[heaters]]
name = "furnace"
driver = "plc_heater"
)");
  ASSERT_TRUE(l.line->start());
  EXPECT_TRUE(l.raised().empty());
  EXPECT_EQ(l.line->switches().info("A")->state, ValveState::Closed);
  ASSERT_TRUE(l.line->actuate("A", systems::SwitchOp::Open, "me"));
  EXPECT_EQ(l.line->switches().info("A")->state, ValveState::Open);
  EXPECT_EQ(l.line->switches().info("B")->state, ValveState::Closed);
  ASSERT_TRUE(l.line->read_gauge("IG1"));
  ASSERT_TRUE(l.line->set_heater_setpoint("furnace", 200.0));
  ASSERT_TRUE(l.line->set_heater_enabled("furnace", true));
  l.scan();
  const auto furnace = l.line->heater_info("furnace");
  ASSERT_TRUE(furnace);
  EXPECT_EQ(*furnace->enabled, true);
  EXPECT_EQ(l.line->switches().info("A")->state, ValveState::Open);  // the heater's coil is its own
  EXPECT_TRUE(l.raised().empty());
}
