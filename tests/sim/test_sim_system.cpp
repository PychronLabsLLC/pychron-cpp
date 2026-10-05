#include "pychron/sim/sim_system.hpp"

#include <chrono>
#include <cmath>

#include <gtest/gtest.h>

#include "pychron/core/config/loader.hpp"
#include "pychron/devices/gp_microion.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/devices/proxr_relay.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace {

using namespace pychron;
using namespace std::chrono_literals;
using sim::SimSystem;

// bone --A-- prep --C-- turbo(pumped)
SimSystem::Topology three_volumes() {
  SimSystem::Topology t;
  t.volumes = {{"bone", 3.0}, {"prep", 1.0}, {"turbo", 1.0}};
  t.valves = {"A", "C"};
  t.edges = {{"bone", "A"}, {"A", "prep"}, {"prep", "C"}, {"C", "turbo"}};
  return t;
}

SimSystem::Settings quiet() {
  SimSystem::Settings s;
  s.default_pressure = 1e-8;
  s.noise = 0.0;
  return s;
}

TEST(SimSystem, ClosedValvesIsolateVolumes) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"bone", 1e-3}};
  SimSystem sim(clock, three_volumes(), settings);

  clock.advance(10s);
  EXPECT_DOUBLE_EQ(*sim.pressure("bone"), 1e-3);
  EXPECT_DOUBLE_EQ(*sim.pressure("prep"), 1e-8);
}

TEST(SimSystem, OpeningAValveEquilibratesByVolume) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"bone", 4e-3}, {"prep", 0.0}};
  SimSystem sim(clock, three_volumes(), settings);

  sim.set_valve("A", true);
  // (3 cc * 4e-3 + 1 cc * 0) / 4 cc
  EXPECT_NEAR(*sim.pressure("bone"), 3e-3, 1e-12);
  EXPECT_NEAR(*sim.pressure("prep"), 3e-3, 1e-12);
  EXPECT_TRUE(sim.valve_open("A"));

  sim.set_valve("A", false);
  sim.set_pressure("prep", 1.0);
  EXPECT_NEAR(*sim.pressure("bone"), 3e-3, 1e-12);
}

TEST(SimSystem, PumpedRegionFollowsPumpDownCurve) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"prep", 1e-2}, {"turbo", 1e-2}};
  settings.pumps = {{"turbo", {1e-9, 2s}}};
  SimSystem sim(clock, three_volumes(), settings);

  sim.set_valve("C", true);
  clock.advance(2s);  // one time constant
  const double expected = 1e-9 + (1e-2 - 1e-9) * std::exp(-1.0);
  EXPECT_NEAR(*sim.pressure("prep"), expected, expected * 1e-9);
  EXPECT_NEAR(*sim.pressure("turbo"), expected, expected * 1e-9);

  // Isolated from the pump, prep stops falling.
  sim.set_valve("C", false);
  const double held = *sim.pressure("prep");
  clock.advance(60s);
  EXPECT_DOUBLE_EQ(*sim.pressure("prep"), held);
  EXPECT_LT(*sim.pressure("turbo"), 1e-8);
}

TEST(SimSystem, UnknownVolumeIsConfigError) {
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto p = sim.pressure("nowhere");
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
}

TEST(SimSystem, UnknownValveIsTrackedButHasNoPhysics) {
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  sim.set_valve("pump_power", true);
  EXPECT_TRUE(sim.valve_open("pump_power"));
  EXPECT_FALSE(sim.valve_open("A"));
}

TEST(SimSystem, GaugeNoiseIsBoundedAndSeeded) {
  ManualClock clock;
  auto settings = quiet();
  settings.noise = 0.01;
  settings.seed = 7;
  SimSystem a(clock, three_volumes(), settings);
  SimSystem b(clock, three_volumes(), settings);
  bool varied = false;
  double first = *a.gauge_reading("prep");
  EXPECT_DOUBLE_EQ(first, *b.gauge_reading("prep"));
  for (int i = 0; i < 50; ++i) {
    double r = *a.gauge_reading("prep");
    EXPECT_NEAR(r, 1e-8, 1e-8 * 0.06);
    varied = varied || r != first;
  }
  EXPECT_TRUE(varied);
}

constexpr const char* kConfig = R"(
[system]
name = "t"

[transports.bus]
kind = "sim"

[transports.gnet]
kind = "sim"

[drivers.relay]
kind = "proxr_relay"
transport = "bus"

[drivers.ig]
kind = "pfeiffer_maxigauge"
transport = "gnet"
channels = [1, 2]

[transports.rs485]
kind = "sim"

[drivers.mi]
kind = "gp_microion"
transport = "rs485"
address = 7
channels = [1, 2]

[[valves]]
name = "A"
actuator = "relay"
address = "1"

[[switches]]
name = "pump_power"
actuator = "relay"
address = "9"

[[gauges]]
name = "IG1"
driver = "ig"
channel = 1

[[gauges]]
name = "MI1"
driver = "mi"
channel = 1
)";

TEST(SimSystem, RelayHookDrivesValveModel) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  auto topo = three_volumes();
  topo.volumes.push_back({"IG1", 1.0});
  SimSystem sim(clock, topo, quiet());

  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("relay"), *cfg));
  ASSERT_TRUE(transport->open());
  ProxrRelay relay("relay", *transport);
  ASSERT_TRUE(relay.open(ValveAddress{"1"}));
  EXPECT_TRUE(sim.valve_open("A"));
  EXPECT_EQ(*relay.read(ValveAddress{"1"}), ValveState::Open);
  ASSERT_TRUE(relay.open(ValveAddress{"9"}));
  EXPECT_TRUE(sim.valve_open("pump_power"));
  ASSERT_TRUE(relay.close(ValveAddress{"1"}));
  EXPECT_FALSE(sim.valve_open("A"));
}

TEST(SimSystem, AnInvertedValveIsOpenWhileItsRelayIsOff) {
  std::string text(kConfig);
  text.replace(text.find("address = \"1\"\n"), 14, "address = \"1\"\ninverted = true\n");
  auto cfg = config::load_system_config_from_string(text, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());

  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("relay"), *cfg));
  ASSERT_TRUE(transport->open());
  ProxrRelay relay("relay", *transport);
  ASSERT_TRUE(relay.close(ValveAddress{"1"}));
  EXPECT_TRUE(sim.valve_open("A"));
  ASSERT_TRUE(relay.open(ValveAddress{"1"}));
  EXPECT_FALSE(sim.valve_open("A"));
}

TEST(SimSystem, MaxiGaugeHookReportsGaugeVolumePressure) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  auto topo = three_volumes();
  topo.volumes.push_back({"IG1", 1.0});
  topo.edges.push_back({"IG1", "prep"});
  auto settings = quiet();
  settings.initial_pressures = {{"prep", 2e-6}, {"IG1", 2e-6}};
  SimSystem sim(clock, topo, settings);

  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("ig"), *cfg));
  ASSERT_TRUE(transport->open());
  PfeifferMaxiGauge gauge("ig", *transport, {1, 2});
  auto p = gauge.read_pressure(1);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, 2e-6, 2e-6 * 1e-3);
  // Channel 2 has no configured gauge: no sensor.
  EXPECT_FALSE(gauge.read_pressure(2));
}

TEST(SimSystem, GaugeNotInTopologyGetsItsOwnVolume) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("ig"), *cfg));
  ASSERT_TRUE(transport->open());
  PfeifferMaxiGauge gauge("ig", *transport, {1});
  auto p = gauge.read_pressure(1);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, 1e-8, 1e-8 * 1e-3);
}

TEST(SimSystem, MicroIonHookReportsGaugeVolumePressureAtItsAddress) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  auto topo = three_volumes();
  topo.volumes.push_back({"MI1", 1.0});
  topo.edges.push_back({"MI1", "prep"});
  auto settings = quiet();
  settings.initial_pressures = {{"prep", 3e-7}, {"MI1", 3e-7}};
  SimSystem sim(clock, topo, settings);

  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("mi"), *cfg));
  ASSERT_TRUE(transport->open());
  GpMicroIon gauge("mi", *transport, 7, {1, 2});
  auto p = gauge.read_pressure(1);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, 3e-7, 3e-7 * 1e-3);
  // Channel 2 has no configured gauge: no sensor.
  EXPECT_FALSE(gauge.read_pressure(2));
  // A driver at the wrong address gets no reply from the simulated slave.
  GpMicroIon other("other", *transport, 8, {1});
  EXPECT_FALSE(other.read_pressure(1));
}

TEST(SimSystem, UnknownDriverKindGetsSilentWire) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto dc = cfg->drivers.at("relay");
  dc.kind = "mystery";
  auto hook = sim.hook_for(dc, *cfg);
  ASSERT_TRUE(hook);
  EXPECT_TRUE(hook(Bytes{0x01}).empty());
}

}  // namespace

// A chromium driver on a sim transport talks to a Chromium simulator, whose
// stage moves on the system's clock.
TEST(SimSystem, AChromiumDriverGetsAChromiumSimulator) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"

[transports.laser_pc]
kind = "sim"

[drivers.laser]
kind = "chromium"
transport = "laser_pc"
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, {}, quiet());
  auto hook = sim.hook_for(cfg->drivers.at("laser"), *cfg);
  ASSERT_TRUE(hook);
  EXPECT_EQ(to_string(hook(to_bytes("Sys.ID?\n"))), "CHROMIUM 2013.12.30.0\r");
  EXPECT_EQ(to_string(hook(to_bytes("Stage.MoveTo 5000,0,0,5000,5000,100\n"))), "");
  clock.advance(std::chrono::seconds(1));
  EXPECT_EQ(to_string(hook(to_bytes("Stage.Pos?\n"))), "5000,0,0\r");
  // the simulator is reachable by its driver's name
  ASSERT_NE(sim.chromium("laser"), nullptr);
  EXPECT_EQ(sim.chromium("laser")->position().x, 5000);
  EXPECT_EQ(sim.chromium("laser_pc"), nullptr);
  EXPECT_EQ(sim.chromium("nope"), nullptr);
}
