#include "pychron/sim/sim_system.hpp"

#include <chrono>
#include <cmath>

#include <gtest/gtest.h>

#include <map>

#include "pychron/devices/heater.hpp"

#include "pychron/core/config/loader.hpp"
#include "pychron/devices/gp_microion.hpp"
#include "pychron/devices/pfeiffer_maxigauge.hpp"
#include "pychron/devices/agilent_switch.hpp"
#include "pychron/devices/channel_gauge.hpp"
#include "pychron/devices/driver_registry.hpp"
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

TEST(SimSystem, AnAgilentUnitDrivesItsValvesWithEitherPolarity) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.agilent]
kind = "sim"
[drivers.switch_controller]
kind = "agilent_switch"
transport = "agilent"
invert = true
[[valves]]
name = "A"
actuator = "switch_controller"
address = "101"
[[valves]]
name = "B"
actuator = "switch_controller"
address = "102"
inverted = true
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("switch_controller"), *cfg));
  ASSERT_TRUE(transport->open());
  AgilentSwitch unit("switch_controller", *transport, /*invert=*/true);
  EXPECT_EQ(*unit.read(ValveAddress{"101"}), ValveState::Closed);  // the line starts closed
  ASSERT_TRUE(unit.open(ValveAddress{"101"}));
  EXPECT_TRUE(sim.valve_open("A"));
  ASSERT_TRUE(unit.close(ValveAddress{"101"}));
  EXPECT_FALSE(sim.valve_open("A"));
  // B is wired backwards on top: the channel the manager closes opens it.
  ASSERT_TRUE(unit.close(ValveAddress{"102"}));
  EXPECT_TRUE(sim.valve_open("B"));
}

TEST(SimSystem, QtegraValvesMoveTheLinesValvesByName) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.qtegra]
kind = "sim"
[drivers.switch_controller]
kind = "qtegra_valves"
transport = "qtegra"
link = "sim_qtegra_valves"
[[valves]]
name = "A"
actuator = "switch_controller"
address = "Valve 1_1 Set"
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("switch_controller"), *cfg));
  ASSERT_TRUE(transport->open());
  auto made = DriverRegistry::global().create(cfg->drivers.at("switch_controller"), *transport);
  ASSERT_TRUE(made) << made.error().what;
  auto* valves = capability<IValveActuator>(**made);
  ASSERT_NE(valves, nullptr);
  EXPECT_EQ(*valves->read(ValveAddress{"Valve 1_1 Set"}), ValveState::Closed);
  ASSERT_TRUE(valves->open(ValveAddress{"Valve 1_1 Set"}));
  EXPECT_TRUE(sim.valve_open("A"));
  ASSERT_TRUE(valves->close(ValveAddress{"Valve 1_1 Set"}));
  EXPECT_FALSE(sim.valve_open("A"));
}

TEST(SimSystem, AnotherPychronServesTheLinesValves) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.felix]
kind = "sim"
[drivers.felix_valves]
kind = "pychron_valves"
transport = "felix"
[[valves]]
name = "A"
actuator = "felix_valves"
address = "A"
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("felix_valves"), *cfg));
  ASSERT_TRUE(transport->open());
  auto made = DriverRegistry::global().create(cfg->drivers.at("felix_valves"), *transport);
  ASSERT_TRUE(made) << made.error().what;
  auto* valves = capability<IValveActuator>(**made);
  ASSERT_TRUE(valves->open(ValveAddress{"A"}));
  EXPECT_TRUE(sim.valve_open("A"));
  auto unknown = valves->open(ValveAddress{"Z"});  // felix does not serve Z
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
}

TEST(SimSystem, AnXgs600ReadsEachGaugeByItsLabel) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.xgs]
kind = "sim"
[drivers.xgs]
kind = "varian_xgs600"
transport = "xgs"
labels = ["CNV1", "IMG1"]
[[gauges]]
name = "IG1"
driver = "xgs"
channel = 2
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  auto topo = three_volumes();
  topo.volumes.push_back({"IG1", 1.0});
  SimSystem sim(clock, topo, quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("xgs"), *cfg));
  ASSERT_TRUE(transport->open());
  auto made = DriverRegistry::global().create(cfg->drivers.at("xgs"), *transport);
  ASSERT_TRUE(made) << made.error().what;
  auto* gauge = capability<IChannelPressureGauge>(**made);
  auto p = gauge->read_pressure(2);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, *sim.gauge_reading("IG1"), *p * 1e-3);
  EXPECT_FALSE(gauge->read_pressure(1));  // CNV1 has no gauge in the line: off
}

TEST(SimSystem, QtegraGaugesReadTheirGaugesVolumes) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.qtegra]
kind = "sim"
[drivers.ms_gauges]
kind = "qtegra_gauges"
transport = "qtegra"
parameters = ["Ion Gauge MS Readback"]
link = "sim_qtegra_gauges"
[[gauges]]
name = "MS_IG"
driver = "ms_gauges"
channel = 1
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("ms_gauges"), *cfg));
  ASSERT_TRUE(transport->open());
  auto made = DriverRegistry::global().create(cfg->drivers.at("ms_gauges"), *transport);
  ASSERT_TRUE(made) << made.error().what;
  auto p = capability<IPressureGauge>(**made)->read_pressure();
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, *sim.gauge_reading("MS_IG"), *p * 1e-6);
}

TEST(SimSystem, APlcHoldsEachGaugesPressureInItsRegisters) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.plc]
kind = "sim"
[drivers.plc_gauges]
kind = "plc2000_gauges"
transport = "plc"
channels = [1, 2]
[[gauges]]
name = "IG1"
driver = "plc_gauges"
channel = 2
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("plc_gauges"), *cfg));
  ASSERT_TRUE(transport->open());
  auto made = DriverRegistry::global().create(cfg->drivers.at("plc_gauges"), *transport);
  ASSERT_TRUE(made) << made.error().what;
  auto* gauge = capability<IChannelPressureGauge>(**made);
  auto p = gauge->read_pressure(2);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, *sim.gauge_reading("IG1"), *p * 1e-6);
  EXPECT_FALSE(gauge->read_pressure(1));  // no gauge there: the PLC has no such register
}

TEST(SimSystem, APlcsCoilsMoveAndReadItsValves) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.plc]
kind = "sim"
[drivers.plc_valves]
kind = "plc2000_valves"
transport = "plc"
[[valves]]
name = "A"
actuator = "plc_valves"
address = "1"
[[valves]]
name = "C"
actuator = "plc_valves"
address = "2"
inverted = true
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("plc_valves"), *cfg));
  ASSERT_TRUE(transport->open());
  auto made = DriverRegistry::global().create(cfg->drivers.at("plc_valves"), *transport);
  ASSERT_TRUE(made) << made.error().what;
  auto* plc = capability<IValveActuator>(**made);
  ASSERT_TRUE(plc->open({"1"}));
  EXPECT_TRUE(sim.valve_open("A"));
  EXPECT_EQ(*plc->read({"1"}), ValveState::Open);
  // C is wired backwards: its coil reads on while the valve is closed.
  EXPECT_EQ(*plc->read({"2"}), ValveState::Open);
  ASSERT_TRUE(plc->close({"2"}));
  EXPECT_TRUE(sim.valve_open("C"));
  auto both = plc->read_many({{"1"}, {"2"}});
  EXPECT_EQ(*both[0], ValveState::Open);
  EXPECT_EQ(*both[1], ValveState::Closed);
  EXPECT_FALSE(plc->read({"9"}));  // no valve there: the PLC has no such coil
}

// AELAMS's one PLC: valves, gauges and a heater behind one Modbus TCP
// connection (plan 2026-10-05, the shared PLC).
TEST(SimSystem, OnePlcAnswersForEveryDriverOnItsTransport) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.plc]
kind = "sim"
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
use_pid = 11
setpoint = 31
readback = 33
[[valves]]
name = "A"
actuator = "plc_valves"
address = "1"
[[gauges]]
name = "IG1"
driver = "plc_gauges"
channel = 21
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for_transport("plc", *cfg));
  ASSERT_TRUE(transport->open());
  std::map<std::string, std::unique_ptr<Device>> devices;
  for (const char* name : {"plc_valves", "plc_gauges", "plc_heater"}) {
    auto made = DriverRegistry::global().create(cfg->drivers.at(name), *transport);
    ASSERT_TRUE(made) << name << ": " << made.error().what;
    devices[name] = std::move(*made);
  }
  auto* valves = capability<IValveActuator>(*devices["plc_valves"]);
  auto* gauges = capability<IChannelPressureGauge>(*devices["plc_gauges"]);
  auto* heater = capability<IHeater>(*devices["plc_heater"]);

  ASSERT_TRUE(valves->open({"1"}));
  EXPECT_TRUE(sim.valve_open("A"));
  EXPECT_EQ(*valves->read({"1"}), ValveState::Open);
  auto p = gauges->read_pressure(21);
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, *sim.gauge_reading("IG1"), *p * 1e-6);
  ASSERT_TRUE(heater->set_setpoint(300.0));
  ASSERT_TRUE(heater->set_enabled(true));
  EXPECT_DOUBLE_EQ(*heater->setpoint(), 300.0);
  EXPECT_TRUE(*heater->enabled());
  ASSERT_NE(sim.heater("plc_heater"), nullptr);
  EXPECT_TRUE(sim.heater("plc_heater")->enabled());
  clock.advance(std::chrono::minutes(10));
  EXPECT_NEAR(*heater->readback(), 300.0, 1.0);
  // The heater's enable coil is not a valve, and the valve's is not the heater's.
  EXPECT_TRUE(*heater->enabled());
  ASSERT_TRUE(valves->close({"1"}));
  EXPECT_TRUE(*heater->enabled());
  EXPECT_FALSE(sim.valve_open("A"));
  // A coil nobody has is still an illegal address.
  EXPECT_FALSE(valves->read({"40"}));
}

TEST(SimSystem, PlcDriversWithDifferentUnitsShareATransport) {
  // A Modbus gateway: each unit answers only for itself.
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.plc]
kind = "sim"
[drivers.plc_valves]
kind = "plc2000_valves"
transport = "plc"
unit = 2
[drivers.plc_heater]
kind = "plc2000_heater"
transport = "plc"
unit = 3
enable = 1
[[valves]]
name = "A"
actuator = "plc_valves"
address = "1"
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for_transport("plc", *cfg));
  ASSERT_TRUE(transport->open());
  auto valves = DriverRegistry::global().create(cfg->drivers.at("plc_valves"), *transport);
  auto heater = DriverRegistry::global().create(cfg->drivers.at("plc_heater"), *transport);
  ASSERT_TRUE(valves && heater);
  ASSERT_TRUE(capability<IValveActuator>(**valves)->open({"1"}));
  EXPECT_TRUE(sim.valve_open("A"));
  ASSERT_TRUE(capability<IHeater>(**heater)->set_enabled(true));
  // Coil 0 of unit 3 is the heater's, not valve A's.
  EXPECT_TRUE(sim.valve_open("A"));
  ASSERT_TRUE(capability<IHeater>(**heater)->set_enabled(false));
  EXPECT_TRUE(sim.valve_open("A"));
}

TEST(SimSystem, AMixedTransportIsHookedForItsFirstDriver) {
  auto cfg = config::load_system_config_from_string(R"(
[system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.a_relay]
kind = "proxr_relay"
transport = "bus"
[drivers.b_plc]
kind = "plc2000_valves"
transport = "bus"
[[valves]]
name = "A"
actuator = "a_relay"
address = "1"
)",
                                                    "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  auto transport = SimTransport::hooked(sim.hook_for_transport("bus", *cfg));
  ASSERT_TRUE(transport->open());
  auto relay = DriverRegistry::global().create(cfg->drivers.at("a_relay"), *transport);
  ASSERT_TRUE(relay);
  ASSERT_TRUE(capability<IValveActuator>(**relay)->open({"1"}));
  EXPECT_TRUE(sim.valve_open("A"));
  EXPECT_FALSE(sim.hook_for_transport("nothing_on_it", *cfg));
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
