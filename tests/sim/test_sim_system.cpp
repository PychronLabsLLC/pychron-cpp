#include "pychron/sim/sim_system.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <map>

#include "pychron/sim/gas.hpp"

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

// No gauge noise, and walls that give nothing off: what is in a volume is
// what was put there.
SimSystem::Settings quiet() {
  SimSystem::Settings s;
  s.default_pressure = 1e-8;
  s.noise = 0.0;
  s.outgassing = 0.0;
  s.outgassing_active = 0.0;
  return s;
}

using sim::Composition;
using sim::Species;
constexpr std::size_t kAr36 = sim::index(Species::Ar36);
constexpr std::size_t kAr40 = sim::index(Species::Ar40);
constexpr std::size_t kActive = sim::index(Species::Active);

// a --V-- b, 50 cc each.
SimSystem::Topology two_volumes() {
  SimSystem::Topology t;
  t.volumes = {{"a", 50.0}, {"b", 50.0}};
  t.valves = {"V"};
  t.edges = {{"a", "V"}, {"V", "b"}};
  return t;
}

// The clock's nearest to `seconds`.
Duration clock_time(double seconds) {
  return std::chrono::duration_cast<Duration>(std::chrono::duration<double>(seconds));
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
  clock.advance(30s);  // thousands of the valve's time constants
  // (3 cc * 4e-3 + 1 cc * 0) / 4 cc
  EXPECT_NEAR(*sim.pressure("bone"), 3e-3, 1e-12);
  EXPECT_NEAR(*sim.pressure("prep"), 3e-3, 1e-12);
  EXPECT_TRUE(sim.valve_open("A"));

  sim.set_valve("A", false);
  sim.set_pressure("prep", 1.0);
  clock.advance(30s);  // an open A would have brought it across
  EXPECT_NEAR(*sim.pressure("bone"), 3e-3, 1e-12);
}

TEST(SimSystem, PumpedRegionFollowsPumpDownCurve) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"prep", 1e-2}, {"turbo", 1e-2}};
  settings.pumps = {{"turbo", {1e-9, 2s}}};
  // The valve is no obstacle here: prep and turbo fall as one volume.
  settings.valve_conductance = 1e6;
  SimSystem sim(clock, three_volumes(), settings);

  sim.set_valve("C", true);
  // One time constant of the region: the pump's 2 s is for turbo alone, and
  // through C it has prep to empty as well, twice the volume.
  clock.advance(4s);
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
    clock.advance(1ms);  // a reading is a draw of its volume and its time
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

TEST(SimSystem, EquilibrationTakesTheValvesTimeConstant) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"a", 1e-6}, {"b", 1e-9}};
  SimSystem sim(clock, two_volumes(), settings);
  const auto difference = [&](std::size_t species) {
    return (*sim.partial_pressures("a"))[species] - (*sim.partial_pressures("b"))[species];
  };
  const double argon_start = difference(kAr40);
  const double active_start = difference(kActive);

  sim.set_valve("V", true);
  EXPECT_DOUBLE_EQ(*sim.pressure("a"), 1e-6);  // nothing has crossed yet
  EXPECT_DOUBLE_EQ(*sim.pressure("b"), 1e-9);

  // V1 V2 / ((V1 + V2) C) for Ar40; the lighter active gas crosses sooner.
  const double argon_tau = 0.05 * 0.05 / (0.1 * 0.1);
  const double active_tau = argon_tau * std::sqrt(28.0 / 39.962);
  ASSERT_DOUBLE_EQ(argon_tau, 0.25);
  clock.advance(clock_time(active_tau));
  EXPECT_NEAR(difference(kActive), active_start / std::exp(1.0), active_start / std::exp(1.0) * 1e-6);
  clock.advance(250ms - clock_time(active_tau));
  EXPECT_NEAR(difference(kAr40), argon_start / std::exp(1.0), argon_start / std::exp(1.0) * 1e-6);
  EXPECT_GT(*sim.pressure("a"), *sim.pressure("b"));

  clock.advance(30s);
  const double mean = (1e-6 + 1e-9) / 2;
  EXPECT_NEAR(*sim.pressure("a"), mean, mean * 1e-12);
  EXPECT_NEAR(*sim.pressure("b"), mean, mean * 1e-12);
}

TEST(SimSystem, GaugeReadingsDoNotDependOnCallOrder) {
  ManualClock one_clock;
  ManualClock other_clock;
  auto settings = quiet();
  settings.noise = 0.01;
  SimSystem one(one_clock, two_volumes(), settings);
  SimSystem other(other_clock, two_volumes(), settings);
  one_clock.advance(3s);
  other_clock.advance(3s);

  const double a_first = *one.gauge_reading("a");
  const double b_second = *one.gauge_reading("b");
  const double b_first = *other.gauge_reading("b");
  const double a_second = *other.gauge_reading("a");
  EXPECT_EQ(a_first, a_second);
  EXPECT_EQ(b_first, b_second);
  // Two volumes at one pressure are still two gauges.
  EXPECT_NE(a_first, b_first);
  // Asking again at the same instant is the same reading; later is another.
  EXPECT_EQ(*one.gauge_reading("a"), a_first);
  one_clock.advance(1ms);
  EXPECT_NE(*one.gauge_reading("a"), a_first);
}

TEST(SimSystem, PumpDownKeepsItsForm) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"line", 1e-6}};
  settings.pumps = {{"line", {1e-9, 5s}}};
  SimSystem::Topology topology;
  topology.volumes = {{"line", 50.0}};
  SimSystem sim(clock, topology, settings);

  clock.advance(5s);
  const double expected = 1e-9 + (1e-6 - 1e-9) / std::exp(1.0);
  EXPECT_NEAR(*sim.pressure("line"), expected, expected * 1e-6);
  clock.advance(10min);
  EXPECT_NEAR(*sim.pressure("line"), 1e-9, 1e-9 * 1e-6);
}

TEST(SimSystem, PartialPressuresFollowTheComposition) {
  ManualClock clock;
  auto settings = quiet();
  const Composition cocktail = sim::with_ar40(sim::cocktail_ratios(), 2e-7);
  settings.compositions = {{"a", cocktail}};
  settings.initial_pressures = {{"a", 1.0}, {"b", 4e-7}};  // a's composition says what a holds
  SimSystem sim(clock, two_volumes(), settings);

  auto a = sim.partial_pressures("a");
  ASSERT_TRUE(a) << a.error().what;
  for (std::size_t i = 0; i < sim::kSpeciesCount; ++i) {
    EXPECT_DOUBLE_EQ((*a)[i], cocktail[i]) << sim::kSpeciesName[i];
  }
  EXPECT_DOUBLE_EQ(*sim.pressure("a"), sim::total(cocktail));

  // A pressure with no composition is that much air.
  auto b = sim.partial_pressures("b");
  ASSERT_TRUE(b) << b.error().what;
  EXPECT_NEAR(sim::total(*b), 4e-7, 4e-7 * 1e-12);
  EXPECT_NEAR((*b)[kAr40] / (*b)[kAr36], 298.56, 298.56 * 1e-12);
  EXPECT_NEAR((*b)[kActive] / sim::total(*b), 1.0 - 1.0 / 107.0, 1e-12);

  auto nowhere = sim.partial_pressures("nowhere");
  ASSERT_FALSE(nowhere);
  EXPECT_EQ(nowhere.error().kind, ErrorKind::Config);
}

TEST(SimSystem, SetPressureScalesTheComposition) {
  ManualClock clock;
  auto settings = quiet();
  const Composition cocktail = sim::with_ar40(sim::cocktail_ratios(), 2e-7);
  settings.compositions = {{"a", cocktail}};
  settings.initial_pressures = {{"b", 0.0}};
  SimSystem sim(clock, two_volumes(), settings);

  ASSERT_TRUE(sim.set_pressure("a", 3.0 * sim::total(cocktail)));
  const Composition a = *sim.partial_pressures("a");
  for (std::size_t i = 0; i < sim::kSpeciesCount; ++i) {
    EXPECT_NEAR(a[i], 3.0 * cocktail[i], 3.0 * cocktail[i] * 1e-12) << sim::kSpeciesName[i];
  }

  // An empty volume has no proportions to keep: it is given air.
  EXPECT_DOUBLE_EQ(*sim.pressure("b"), 0.0);
  ASSERT_TRUE(sim.set_pressure("b", 1e-6));
  const Composition b = *sim.partial_pressures("b");
  EXPECT_NEAR(sim::total(b), 1e-6, 1e-6 * 1e-12);
  EXPECT_NEAR(b[kAr40] / b[kAr36], 298.56, 298.56 * 1e-12);

  // A composition is taken as given.
  ASSERT_TRUE(sim.set_composition("b", cocktail));
  EXPECT_EQ(*sim.partial_pressures("b"), cocktail);

  for (const double bad : {-1.0, std::nan("")}) {
    auto refused = sim.set_pressure("a", bad);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  }
  EXPECT_FALSE(sim.set_composition("nowhere", cocktail));
  EXPECT_NEAR(*sim.pressure("a"), 3.0 * sim::total(cocktail), sim::total(cocktail) * 1e-12);
}

TEST(SimSystem, InjectReachesTheGauge) {
  ManualClock clock;
  auto settings = quiet();
  SimSystem sim(clock, two_volumes(), settings);
  const double before = *sim.gauge_reading("a");

  Composition released{};
  released[kAr40] = 5e-9;  // mbar L, into 50 cc
  ASSERT_TRUE(sim.inject("a", released));
  EXPECT_NEAR(*sim.gauge_reading("a"), before + 1e-7, 1e-7 * 1e-12);
  clock.advance(30s);  // V is closed: none of it reaches b, however long
  EXPECT_NEAR((*sim.partial_pressures("a"))[kAr40] - (*sim.partial_pressures("b"))[kAr40], 1e-7, 1e-7 * 1e-12);
  EXPECT_DOUBLE_EQ(*sim.gauge_reading("b"), before);

  // And, the valve open, the gauge on the other side.
  sim.set_valve("V", true);
  clock.advance(30s);
  EXPECT_NEAR(*sim.gauge_reading("b"), before + 0.5e-7, 1e-7 * 1e-12);

  released[kAr40] = -1.0;
  EXPECT_FALSE(sim.inject("a", released));
  EXPECT_FALSE(sim.inject("nowhere", Composition{}));
}

// Review focus 4: a switch, or a valve the canvas does not draw.
TEST(SimSystem, AnUnmodelledValveIsTrackedWithoutPhysics) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"a", 1e-6}, {"b", 1e-9}};
  SimSystem sim(clock, two_volumes(), settings);
  EXPECT_FALSE(sim.valve_open("pump_power"));

  sim.set_valve("pump_power", true);
  EXPECT_TRUE(sim.valve_open("pump_power"));
  EXPECT_FALSE(sim.valve_open("V"));
  clock.advance(30s);
  EXPECT_DOUBLE_EQ(*sim.pressure("a"), 1e-6);
  EXPECT_DOUBLE_EQ(*sim.pressure("b"), 1e-9);
  EXPECT_FALSE(sim.has_volume("pump_power"));

  sim.set_valve("pump_power", false);
  EXPECT_FALSE(sim.valve_open("pump_power"));
  EXPECT_FALSE(sim.build_error());
}

TEST(SimSystem, ABadTopologyIsReportedNotThrown) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"bone", -1e-3}};
  SimSystem sim(clock, three_volumes(), settings);

  ASSERT_TRUE(sim.build_error());
  EXPECT_EQ(sim.build_error()->kind, ErrorKind::Config);
  EXPECT_NE(sim.build_error()->what.find("bone"), std::string::npos) << sim.build_error()->what;

  // A line with nothing in it: no volume answers, valves are still tracked.
  EXPECT_FALSE(sim.has_volume("bone"));
  auto p = sim.pressure("prep");
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
  EXPECT_FALSE(sim.gauge_reading("prep"));
  EXPECT_FALSE(sim.set_pressure("prep", 1e-6));
  sim.set_valve("A", true);
  clock.advance(1s);
  EXPECT_TRUE(sim.valve_open("A"));

  // A gauge is still given its own volume.
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("ig"), *cfg));
  ASSERT_TRUE(transport->open());
  PfeifferMaxiGauge gauge("ig", *transport, {1});
  auto read = gauge.read_pressure(1);
  ASSERT_TRUE(read) << read.error().what;
  EXPECT_NEAR(*read, 1e-8, 1e-8 * 1e-3);
}

TEST(SimSystem, ARefusedGaugeVolumeIsReported) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"IG1", -1e-6}, {"MI1", -2e-6}};  // gauges the topology does not have
  SimSystem sim(clock, three_volumes(), settings);
  ASSERT_FALSE(sim.build_error()) << sim.build_error()->what;  // the line itself is sound

  auto transport = SimTransport::hooked(sim.hook_for(cfg->drivers.at("ig"), *cfg));
  ASSERT_TRUE(sim.build_error());
  EXPECT_EQ(sim.build_error()->kind, ErrorKind::Config);
  EXPECT_NE(sim.build_error()->what.find("IG1"), std::string::npos) << sim.build_error()->what;
  // The gauge has no volume and reads nothing; the line is as it was.
  EXPECT_FALSE(sim.has_volume("IG1"));
  ASSERT_TRUE(transport->open());
  PfeifferMaxiGauge gauge("ig", *transport, {1});
  EXPECT_FALSE(gauge.read_pressure(1));
  EXPECT_DOUBLE_EQ(*sim.pressure("prep"), 1e-8);

  // The first refusal is the one kept.
  (void)sim.hook_for(cfg->drivers.at("mi"), *cfg);
  EXPECT_NE(sim.build_error()->what.find("IG1"), std::string::npos) << sim.build_error()->what;
}

TEST(SimSystem, NoiseThatIsNoNumberIsNoNoise) {
  for (const double noise : {std::nan(""), -0.01, std::numeric_limits<double>::infinity()}) {
    ManualClock clock;
    auto settings = quiet();
    settings.noise = noise;
    SimSystem sim(clock, three_volumes(), settings);
    clock.advance(3s);
    EXPECT_EQ(*sim.gauge_reading("prep"), *sim.pressure("prep")) << noise;
    EXPECT_DOUBLE_EQ(*sim.gauge_reading("prep"), 1e-8) << noise;
  }
}

// What a description could always get wrong and still give a line.
TEST(SimSystem, ALooseDescriptionStillBuilds) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"bone", 4e-3}, {"prep", 0.0}, {"nowhere", 1.0}};
  settings.pumps = {{"nowhere", {1e-9, 1s}}, {"turbo", {1e-9, 0s}}};
  auto topology = three_volumes();
  topology.volumes.push_back({"bone", 7.0});       // again: the first stands
  topology.volumes.push_back({"unsized", 0.0});    // takes the default size
  topology.valves.push_back("A");                  // again
  topology.valves.push_back("prep");               // the name of a volume
  topology.edges.push_back({"bone", "nowhere"});   // to nothing
  topology.edges.push_back({"bone", "bone"});      // to itself
  SimSystem sim(clock, topology, settings);
  ASSERT_FALSE(sim.build_error()) << sim.build_error()->what;

  sim.set_valve("A", true);
  clock.advance(30s);
  EXPECT_NEAR(*sim.pressure("bone"), 3e-3, 1e-12);  // 3 cc and 1 cc, as described first
  EXPECT_NEAR(*sim.pressure("prep"), 3e-3, 1e-12);
  EXPECT_TRUE(sim.has_volume("unsized"));
  // A pump with no time constant is at its base at once.
  EXPECT_NEAR(*sim.pressure("turbo"), 1e-9, 1e-9 * 1e-6);
}

TEST(SimSystem, AnIsolatedVolumeRisesByItsOutgassing) {
  ManualClock clock;
  SimSystem::Settings settings;  // the defaults: walls give off gas
  settings.noise = 0.0;
  SimSystem sim(clock, two_volumes(), settings);
  const Composition start = *sim.partial_pressures("a");

  clock.advance(1000s);
  const Composition later = *sim.partial_pressures("a");
  // Per litre of volume, so the same rise of pressure whatever the size.
  EXPECT_NEAR(later[kAr40] - start[kAr40], 5e-13 * 1000, 5e-13 * 1000 * 1e-6);
  EXPECT_NEAR(later[kAr36] - start[kAr36], 5e-13 * 1000 / 298.56, 5e-13 * 1000 / 298.56 * 1e-6);
  EXPECT_NEAR(later[kActive] - start[kActive], 1e-10 * 1000, 1e-10 * 1000 * 1e-6);
}

TEST(SimSystem, ALeakAGetterAndAValvesOwnConductanceAreTheSettings) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"a", 1e-6}, {"b", 1e-6}};
  settings.leaks = {{"a", 1e-12}};   // mbar L / s of Ar40, the rest as in air
  settings.getters = {{"b", true}};
  settings.conductances = {{"V", 0.05}};
  SimSystem sim(clock, two_volumes(), settings);
  const Composition a = *sim.partial_pressures("a");
  const Composition b = *sim.partial_pressures("b");

  clock.advance(100s);
  const Composition leaked = *sim.partial_pressures("a");
  EXPECT_NEAR(leaked[kAr40] - a[kAr40], 1e-12 * 100 / 0.05, 1e-12 * 100 / 0.05 * 1e-6);
  EXPECT_NEAR((leaked[kAr40] - a[kAr40]) / (leaked[kAr36] - a[kAr36]), 298.56, 298.56 * 1e-6);
  const Composition gettered = *sim.partial_pressures("b");
  EXPECT_LT(gettered[kActive], b[kActive] * 1e-9);
  EXPECT_NEAR(gettered[kAr40], b[kAr40], b[kAr40] * 1e-12);

  // Half the conductance, twice the time constant.
  ASSERT_TRUE(sim.set_composition("a", sim::with_ar40(sim::cocktail_ratios(), 1e-6)));
  ASSERT_TRUE(sim.set_composition("b", Composition{}));
  sim.set_valve("V", true);
  clock.advance(500ms);
  const double difference = (*sim.partial_pressures("a"))[kAr40] - (*sim.partial_pressures("b"))[kAr40];
  EXPECT_NEAR(difference, 1e-6 / std::exp(1.0), 1e-6 / std::exp(1.0) * 1e-4);  // the leak is still there
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

// A line as the canvas gives it: stages with their kinds, sizes mostly unset.
//   tank --T-- pipette --L-- line --I-- source --Q-- ion
//                             |\
//                             G P
//                             |  \
//                           getter turbo
SimSystem::Topology a_lab() {
  using sim::SimRole;
  SimSystem::Topology t;
  t.volumes = {{"tank", 1000.0, SimRole::Tank},
               {"pipette", 0.0, SimRole::Pipette},
               {"line", 0.0, SimRole::Plain},
               {"getter", 0.0, SimRole::Getter},
               {"turbo", 0.0, SimRole::Pump},
               {"source", 0.0, SimRole::Spectrometer},
               {"ion", 0.0, SimRole::Pump}};
  t.valves = {"T", "L", "G", "P", "I", "Q"};
  t.edges = {{"tank", "T"},   {"T", "pipette"}, {"pipette", "L"}, {"L", "line"},   {"line", "G"},   {"G", "getter"},
             {"line", "P"},   {"P", "turbo"},   {"line", "I"},    {"I", "source"}, {"source", "Q"}, {"Q", "ion"}};
  return t;
}

TEST(SimSystem, ATankStartsFullOfAir) {
  ManualClock clock;
  SimSystem sim(clock, a_lab(), quiet());
  const Composition tank = *sim.partial_pressures("tank");
  EXPECT_EQ(tank, sim::with_ar40(sim::air_ratios(), 3e-5));
  EXPECT_DOUBLE_EQ(tank[kAr40] / tank[kAr36], 298.56);
  EXPECT_NEAR(*sim.pressure("line"), 1e-8, 1e-20);  // and nothing else does

  // Unless it is said what it holds, either way.
  auto settings = quiet();
  settings.tank_argon40 = 1e-6;
  SimSystem lean(clock, a_lab(), settings);
  EXPECT_EQ((*lean.partial_pressures("tank"))[kAr40], 1e-6);
  settings.compositions = {{"tank", sim::with_ar40(sim::cocktail_ratios(), 2e-7)}};
  SimSystem cocktail(clock, a_lab(), settings);
  EXPECT_EQ(*cocktail.partial_pressures("tank"), sim::with_ar40(sim::cocktail_ratios(), 2e-7));
  settings.compositions.clear();
  settings.initial_pressures = {{"tank", 5e-4}};
  SimSystem set(clock, a_lab(), settings);
  EXPECT_DOUBLE_EQ(*set.pressure("tank"), 5e-4);
}

TEST(SimSystem, APipetteIsSmallAndAShotIsWhatItHeld) {
  ManualClock clock;
  auto settings = quiet();
  settings.compositions = {{"pipette", Composition{}}, {"line", Composition{}}};
  SimSystem sim(clock, a_lab(), settings);
  const double tank = (*sim.partial_pressures("tank"))[kAr40];

  // Load: 0.1 cc opened to a litre.
  sim.set_valve("T", true);
  clock.advance(10s);
  sim.set_valve("T", false);
  const double loaded = (*sim.partial_pressures("pipette"))[kAr40];
  EXPECT_NEAR(loaded, tank * 1000.0 / 1000.1, tank * 1e-9);
  // Deliver: into the line's 50 cc.
  sim.set_valve("L", true);
  clock.advance(10s);
  EXPECT_NEAR((*sim.partial_pressures("line"))[kAr40], loaded * 0.1 / 50.1, loaded * 0.1 / 50.1 * 1e-9);

  // A size of its own goes first: the settings', then the topology's.
  auto topology = a_lab();
  topology.volumes[1].cc = 0.2;
  settings.sizes = {{"line", 99.8}};
  SimSystem sized(clock, topology, settings);
  ASSERT_TRUE(sized.set_composition("pipette", sim::with_ar40(sim::air_ratios(), 1e-6)));
  sized.set_valve("L", true);
  clock.advance(10s);
  EXPECT_NEAR((*sized.partial_pressures("line"))[kAr40], 1e-6 * 0.2 / 100.0, 1e-6 * 0.2 / 100.0 * 1e-9);
}

TEST(SimSystem, APumpStagePumps) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"turbo", 1e-3}, {"line", 1e-3}};
  SimSystem sim(clock, a_lab(), settings);
  // 50 L/s on its own 50 cc: a millisecond is one time constant.
  clock.advance(1ms);
  const double expected = 1e-9 + (1e-3 - 1e-9) * std::exp(-1.0);
  EXPECT_NEAR(*sim.pressure("turbo"), expected, expected * 1e-9);
  clock.advance(1s);
  EXPECT_NEAR(*sim.pressure("turbo"), 1e-9, 1e-15);
  EXPECT_DOUBLE_EQ(*sim.pressure("line"), 1e-3) << "valved off from it";

  // The settings' speed and base, and a pump of the settings' own on it.
  settings.pump_speed = 5.0;
  settings.pump_base = 1e-7;
  SimSystem slow(clock, a_lab(), settings);
  clock.advance(10ms);
  const double slower = 1e-7 + (1e-3 - 1e-7) * std::exp(-1.0);
  EXPECT_NEAR(*slow.pressure("turbo"), slower, slower * 1e-9);
  settings.pumps = {{"turbo", {1e-8, 2s}}};
  SimSystem own(clock, a_lab(), settings);
  clock.advance(2s);
  const double owned = 1e-8 + (1e-3 - 1e-8) * std::exp(-1.0);
  EXPECT_NEAR(*own.pressure("turbo"), owned, owned * 1e-9) << "its own pump, and not the stage's as well";
  settings.pump_speeds = {{"turbo", 0.05}};  // L/s on 50 cc: a second
  SimSystem given(clock, a_lab(), settings);
  clock.advance(1s);
  EXPECT_NEAR(*given.pressure("turbo"), owned, owned * 1e-9);
}

TEST(SimSystem, AGetterStageRemovesActiveGas) {
  ManualClock clock;
  auto settings = quiet();
  settings.initial_pressures = {{"getter", 1e-5}, {"line", 1e-5}};
  SimSystem sim(clock, a_lab(), settings);
  const Composition before = *sim.partial_pressures("getter");
  // 1 L/s on 50 cc: 50 ms is one time constant.
  clock.advance(50ms);
  const Composition after = *sim.partial_pressures("getter");
  EXPECT_NEAR(after[kActive], before[kActive] * std::exp(-1.0), before[kActive] * 1e-9);
  EXPECT_DOUBLE_EQ(after[kAr40], before[kAr40]);
  EXPECT_DOUBLE_EQ(after[kAr36], before[kAr36]);
  EXPECT_DOUBLE_EQ((*sim.partial_pressures("line"))[kActive], before[kActive]) << "no getter there";

  // The settings' speed; and the old map agrees with the role: it may say a
  // getter stage is none, or make one of a plain volume.
  settings.getter_speed = 0.1;
  settings.getters = {{"line", true}};
  SimSystem slow(clock, a_lab(), settings);
  clock.advance(500ms);
  EXPECT_NEAR((*slow.partial_pressures("getter"))[kActive], before[kActive] * std::exp(-1.0), before[kActive] * 1e-9);
  EXPECT_NEAR((*slow.partial_pressures("line"))[kActive], before[kActive] * std::exp(-1.0), before[kActive] * 1e-9);
  settings.getters = {{"getter", false}};
  SimSystem none(clock, a_lab(), settings);
  clock.advance(10s);
  EXPECT_DOUBLE_EQ((*none.partial_pressures("getter"))[kActive], before[kActive]);
}

TEST(SimSystem, TheSpectrometerStageConsumesAndRemembers) {
  ManualClock clock;
  // Static: valved off from everything. A large signal falls by what the
  // source uses of it.
  auto settings = quiet();
  settings.source.memory_fa_per_s = 0.0;
  settings.compositions = {{"source", sim::with_ar40(sim::air_ratios(), 1e-8)}, {"line", sim::with_ar40(sim::air_ratios(), 1e-8)}};
  SimSystem falling(clock, a_lab(), settings);
  const Composition before = *falling.partial_pressures("source");
  clock.advance(600s);
  const Composition after = *falling.partial_pressures("source");
  const double fall = 1e-8 * 2e-5 * 600;
  EXPECT_NEAR(before[kAr40] - after[kAr40], fall, fall * 0.01);
  EXPECT_NEAR(after[kAr40], 1e-8 * std::exp(-2e-5 * 600), 1e-8 * 1e-12);
  EXPECT_NEAR(after[kAr36] / before[kAr36], after[kAr40] / before[kAr40], 1e-12) << "each argon alike";
  EXPECT_DOUBLE_EQ(after[kActive], before[kActive]) << "and not the active gas";
  EXPECT_EQ(*falling.partial_pressures("line"), settings.compositions.at("line")) << "nor any other volume";

  // From nothing it rises by its memory: 0.01 fA/s at 1e12 fA/mbar.
  settings = quiet();
  settings.compositions = {{"source", Composition{}}};
  SimSystem rising(clock, a_lab(), settings);
  clock.advance(600s);
  const Composition risen = *rising.partial_pressures("source");
  const double rise = 0.01 * 600 / 1e12;
  EXPECT_NEAR(risen[kAr40], rise, rise * 0.01);
  EXPECT_EQ(risen[kAr36], 0.0);
  EXPECT_EQ(risen[kActive], 0.0);

  // Whatever the source's size: memory is a signal, so a pressure.
  settings.sizes = {{"source", 500.0}};
  settings.source = {1e11, 0.0, 0.5};
  SimSystem large(clock, a_lab(), settings);
  clock.advance(100s);
  EXPECT_NEAR((*large.partial_pressures("source"))[kAr40], 0.5 * 100 / 1e11, 0.5 * 100 / 1e11 * 1e-9);
}

// The abundance of `isotope` in what a beam_gas() function answered.
double abundance(const std::vector<sim::BeamGas>& gas, std::string_view isotope) {
  for (const auto& g : gas) {
    if (g.isotope == isotope) return g.abundance;
  }
  ADD_FAILURE() << "no " << isotope;
  return 0.0;
}

TEST(SimSystem, BeamGasIsPartialPressureTimesSensitivity) {
  ManualClock clock;
  auto settings = quiet();
  settings.source = {2e11, 0.0, 0.0};  // no consumption, no memory: the source keeps what it has
  Composition held{};
  held[kAr36] = 1e-10;
  held[sim::index(Species::Ar37)] = 2e-10;
  held[sim::index(Species::Ar38)] = 3e-10;
  held[sim::index(Species::Ar39)] = 4e-10;
  held[kAr40] = 5e-8;
  held[kActive] = 1e-6;  // which no detector sees
  settings.compositions = {{"source", held}};
  SimSystem sim(clock, a_lab(), settings);
  EXPECT_EQ(sim.settings().source.sensitivity, 2e11);

  const auto gas_at = sim.beam_gas();
  ASSERT_TRUE(gas_at);
  const std::vector<sim::BeamGas> gas = gas_at(clock.now());
  // The five argon isotopes, with the beam's own masses and no rate.
  const std::vector<sim::BeamGas> argon = sim::default_argon_gas();
  ASSERT_EQ(gas.size(), argon.size());
  for (std::size_t i = 0; i < gas.size(); ++i) {
    EXPECT_EQ(gas[i].isotope, argon[i].isotope);
    EXPECT_EQ(gas[i].mass, argon[i].mass);
    EXPECT_EQ(gas[i].rate_per_s, 0.0);
  }
  EXPECT_DOUBLE_EQ(abundance(gas, "Ar36"), 1e-10 * 2e11);
  EXPECT_DOUBLE_EQ(abundance(gas, "Ar37"), 2e-10 * 2e11);
  EXPECT_DOUBLE_EQ(abundance(gas, "Ar38"), 3e-10 * 2e11);
  EXPECT_DOUBLE_EQ(abundance(gas, "Ar39"), 4e-10 * 2e11);
  EXPECT_DOUBLE_EQ(abundance(gas, "Ar40"), 5e-8 * 2e11);

  // It follows the volume.
  Composition less = held;
  less[kAr40] = 1e-9;
  ASSERT_TRUE(sim.set_composition("source", less));
  EXPECT_DOUBLE_EQ(abundance(gas_at(clock.now()), "Ar40"), 1e-9 * 2e11);
}

TEST(SimSystem, BeamGasIsEmptyWithoutASpectrometerStage) {
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  EXPECT_FALSE(sim.beam_gas());
  // Nor of a line the network refused.
  auto settings = quiet();
  settings.initial_pressures = {{"source", -1.0}};
  SimSystem refused(clock, a_lab(), settings);
  ASSERT_TRUE(refused.build_error());
  EXPECT_FALSE(refused.beam_gas());
}

// A reading's instant is not the clock's: the model goes to it, but not past
// the clock and not back.
TEST(SimSystem, BeamGasReadsTheInstantItIsAskedFor) {
  ManualClock clock;
  auto settings = quiet();
  settings.source = {1e12, 1e-3, 0.0};  // the source uses a thousandth of its argon a second
  settings.compositions = {{"source", sim::with_ar40(sim::air_ratios(), 1e-8)}};
  SimSystem sim(clock, a_lab(), settings);
  const auto gas_at = sim.beam_gas();
  ASSERT_TRUE(gas_at);
  const TimePoint start = clock.now();
  const auto expected = [](double seconds) { return 1e-8 * 1e12 * std::exp(-1e-3 * seconds); };

  clock.advance(100s);
  // Stamped before now, and after where the model is: the gas of that instant.
  EXPECT_NEAR(abundance(gas_at(start + 40s), "Ar40"), expected(40), expected(40) * 1e-9);
  // Stamped before where the model now is: what is there, since it cannot go back.
  EXPECT_NEAR(abundance(gas_at(start + 10s), "Ar40"), expected(40), expected(40) * 1e-9);
  // Stamped after now: the line is not taken past its own clock.
  EXPECT_NEAR(abundance(gas_at(start + 1h), "Ar40"), expected(100), expected(100) * 1e-9);
  EXPECT_NEAR((*sim.partial_pressures("source"))[kAr40] * 1e12, expected(100), expected(100) * 1e-9);
  // Which then goes on from there as the clock does.
  clock.advance(100s);
  EXPECT_NEAR(abundance(gas_at(clock.now()), "Ar40"), expected(200), expected(200) * 1e-9);
}

// A beam may be read after its line is gone (a registry not yet cleared): it
// then has no gas, and its baseline.
TEST(SimSystem, ABeamFedByALineThatIsGoneReadsNoGas) {
  ManualClock clock;
  sim::BeamModel beam(clock);
  beam.ensure_detector("H1");
  ASSERT_TRUE(beam.set_baseline("H1", 50.0, 0.0));
  const double peak = *beam.peak_center("H1", "Ar40");
  beam.set_magnet(peak);
  {
    auto settings = quiet();
    settings.compositions = {{"source", sim::with_ar40(sim::air_ratios(), 1e-8)}};
    SimSystem sim(clock, a_lab(), settings);
    beam.set_gas_provider(sim.beam_gas());
    EXPECT_NEAR(beam.intensity("H1")->value, 1e4 + 50.0, 5.0 * (1.0 + 0.001 * 1e4));
  }
  clock.advance(1s);
  const auto late = beam.intensity("H1");
  ASSERT_TRUE(late);
  EXPECT_NEAR(late->value, 50.0, 5.0);
  // Where the peak is does not need the line.
  EXPECT_DOUBLE_EQ(*beam.peak_center("H1", "Ar40"), peak);
}

TEST(SimSystem, EveryVolumeOfARoleStillHasItsWalls) {
  ManualClock clock;
  auto settings = quiet();
  settings.outgassing = 5e-13;
  settings.source = {1e12, 0.0, 0.0};
  settings.compositions = {{"pipette", Composition{}}, {"source", Composition{}}, {"line", Composition{}}};
  settings.leaks = {{"line", 1e-12}};
  SimSystem sim(clock, a_lab(), settings);
  clock.advance(1000s);
  // Per litre of each, so the same pressure in each; and the leak on top.
  EXPECT_NEAR((*sim.partial_pressures("pipette"))[kAr40], 5e-10, 5e-10 * 1e-9);
  EXPECT_NEAR((*sim.partial_pressures("source"))[kAr40], 5e-10, 5e-10 * 1e-9);
  EXPECT_NEAR((*sim.partial_pressures("line"))[kAr40], 5e-10 + 1e-12 * 1000 / 0.05, 2.05e-8 * 1e-9);
}

TEST(SimSystem, NoSpectrometerStageMeansNoSpectrometerVolume) {
  ManualClock clock;
  SimSystem sim(clock, three_volumes(), quiet());
  EXPECT_FALSE(sim.spectrometer_volume());
  SimSystem lab(clock, a_lab(), quiet());
  ASSERT_TRUE(lab.spectrometer_volume());
  EXPECT_EQ(*lab.spectrometer_volume(), "source");
}

TEST(SimSystem, TwoSpectrometerStagesTakeTheFirstByName) {
  ManualClock clock;
  auto topology = a_lab();
  topology.volumes.push_back({"argus", 0.0, sim::SimRole::Spectrometer});
  topology.volumes.push_back({"zeta", 0.0, sim::SimRole::Spectrometer});
  SimSystem sim(clock, topology, quiet());
  ASSERT_TRUE(sim.spectrometer_volume());
  EXPECT_EQ(*sim.spectrometer_volume(), "argus");
  // Both are sources to the gas all the same.
  ASSERT_TRUE(sim.set_composition("zeta", sim::with_ar40(sim::air_ratios(), 1e-8)));
  clock.advance(600s);
  EXPECT_LT((*sim.partial_pressures("zeta"))[kAr40], 1e-8 * 0.99);

  // A line the network refused has none.
  auto settings = quiet();
  settings.default_pressure = -1.0;
  SimSystem refused(clock, topology, settings);
  ASSERT_TRUE(refused.build_error());
  EXPECT_FALSE(refused.spectrometer_volume());
}

TEST(SimSystem, SaysWhichValvesHaveNoPhysics) {
  ManualClock clock;
  auto topology = three_volumes();
  topology.valves.push_back("X");
  topology.valves.push_back("Y");
  topology.edges.emplace_back("bone", "X");
  topology.edges.emplace_back("X", "Y");
  topology.edges.emplace_back("Y", "turbo");
  SimSystem sim(clock, topology, quiet());
  const auto without = sim.valves_without_physics();
  ASSERT_EQ(without.size(), 2u);
  EXPECT_EQ(without[0].first, "X");
  EXPECT_NE(without[0].second.find("valve 'Y'"), std::string::npos) << without[0].second;
  EXPECT_EQ(without[1].first, "Y");
  SimSystem sound(clock, three_volumes(), quiet());
  EXPECT_TRUE(sound.valves_without_physics().empty());
}

// A gauge off the canvas gets its volume when its hook is made, and what the
// settings say of its name with it.
TEST(SimSystem, AGaugeVolumeOffTheCanvasHasItsSettings) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ManualClock clock;
  auto settings = quiet();
  settings.outgassing = 5e-13;  // not for it: it has no walls of the line
  settings.initial_pressures = {{"IG1", 1e-3}, {"MI1", 1e-6}};
  settings.pumps = {{"IG1", {1e-9, 5s}}};
  settings.pump_speeds = {{"IG1", 0.02}};
  settings.sizes = {{"IG1", 20.0}, {"MI1", 10.0}};
  settings.leaks = {{"MI1", 1e-12}};
  settings.getters = {{"MI1", true}};
  SimSystem sim(clock, three_volumes(), settings);
  ASSERT_FALSE(sim.has_volume("IG1"));
  (void)sim.hook_for(cfg->drivers.at("ig"), *cfg);
  (void)sim.hook_for(cfg->drivers.at("mi"), *cfg);
  ASSERT_FALSE(sim.build_error()) << sim.build_error()->what;
  ASSERT_TRUE(sim.has_volume("IG1"));
  ASSERT_TRUE(sim.has_volume("MI1"));

  const Composition before = *sim.partial_pressures("MI1");
  clock.advance(1s);  // 0.02 L/s on 20 cc: one time constant
  const double expected = 1e-9 + (1e-3 - 1e-9) * std::exp(-1.0);
  EXPECT_NEAR(*sim.pressure("IG1"), expected, expected * 1e-9);
  const Composition after = *sim.partial_pressures("MI1");
  EXPECT_NEAR(after[kAr40] - before[kAr40], 1e-12 * 1 / 0.01, 1e-10 * 1e-6) << "its leak, into its 10 cc";
  // Its getter, 1 L/s: the active gas it had is gone, and what the leak
  // brings stands at leak / speed.
  const double leaking = sim::with_ar40(sim::air_ratios(), 1e-12)[kActive];
  EXPECT_NEAR(after[kActive], leaking / 1.0, leaking * 1e-6);
  EXPECT_LT(after[kActive], before[kActive] * 1e-3);
}

// A gauge drawn on the canvas is a small volume, as one off the canvas is:
// the one setting sizes both.
TEST(SimSystem, ACanvasGaugeIsSmall) {
  auto cfg = config::load_system_config_from_string(kConfig, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  // line (10 cc) --V-- IG (a gauge, no size)
  SimSystem::Topology t;
  t.volumes = {{"line", 10.0}, {"IG", 0.0, sim::SimRole::Gauge}};
  t.valves = {"V"};
  t.edges = {{"line", "V"}, {"V", "IG"}};
  const auto shared = [&](SimSystem::Settings settings) {
    ManualClock clock;
    settings.initial_pressures = {{"line", 1e-4}};
    SimSystem sim(clock, t, settings);
    sim.set_valve("V", true);
    clock.advance(10s);
    return *sim.pressure("IG");
  };
  EXPECT_EQ(SimSystem::Settings{}.gauge_cc, 1.0);
  EXPECT_NEAR(shared(quiet()), (10 * 1e-4 + 1 * 1e-8) / 11, 1e-4 * 1e-9) << "1 cc, not the default 50";
  auto larger = quiet();
  larger.gauge_cc = 4.0;
  EXPECT_NEAR(shared(larger), (10 * 1e-4 + 4 * 1e-8) / 14, 1e-4 * 1e-9);
  larger.sizes = {{"IG", 2.0}};  // its own size goes before the role's
  EXPECT_NEAR(shared(larger), (10 * 1e-4 + 2 * 1e-8) / 12, 1e-4 * 1e-9);

  // Off the canvas: a pump of 0.004 L/s empties gauge_cc = 4 cc with a time
  // constant of a second.
  ManualClock clock;
  auto settings = quiet();
  settings.gauge_cc = 4.0;
  settings.initial_pressures = {{"IG1", 1e-3}};
  settings.pumps = {{"IG1", {1e-9, 5s}}};
  settings.pump_speeds = {{"IG1", 0.004}};
  SimSystem sim(clock, three_volumes(), settings);
  (void)sim.hook_for(cfg->drivers.at("ig"), *cfg);
  ASSERT_TRUE(sim.has_volume("IG1"));
  clock.advance(1s);
  const double expected = 1e-9 + (1e-3 - 1e-9) * std::exp(-1.0);
  EXPECT_NEAR(*sim.pressure("IG1"), expected, expected * 1e-9);
}

// The pipe the line puts between two valves it finds joined directly.
TEST(SimSystem, APipeIsSmall) {
  // a (10 cc) --V-- a~b (a pipe, no size)
  SimSystem::Topology t;
  t.volumes = {{"a", 10.0}, {"a~b", 0.0, sim::SimRole::Pipe}};
  t.valves = {"V"};
  t.edges = {{"a", "V"}, {"V", "a~b"}};
  const auto shared = [&](SimSystem::Settings settings) {
    ManualClock clock;
    settings.initial_pressures = {{"a", 1e-4}};
    SimSystem sim(clock, t, settings);
    sim.set_valve("V", true);
    clock.advance(10s);
    return *sim.pressure("a~b");
  };
  EXPECT_EQ(SimSystem::Settings{}.pipe_cc, 1.0);
  EXPECT_NEAR(shared(quiet()), (10 * 1e-4 + 1 * 1e-8) / 11, 1e-4 * 1e-9);
  auto wider = quiet();
  wider.pipe_cc = 5.0;
  EXPECT_NEAR(shared(wider), (10 * 1e-4 + 5 * 1e-8) / 15, 1e-4 * 1e-9);
  wider.sizes = {{"a~b", 2.0}};
  EXPECT_NEAR(shared(wider), (10 * 1e-4 + 2 * 1e-8) / 12, 1e-4 * 1e-9);
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
