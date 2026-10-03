// SimValves: the in-memory valve actuator legacy-line imports stand in with.

#include "pychron/devices/sim_valves.hpp"

#include <gtest/gtest.h>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;

namespace {

ValveState state(const Result<ValveState>& r) { return r ? *r : ValveState::Unknown; }

TEST(SimValves, EveryValveStartsClosedAndKeepsItsState) {
  SimValves v("actuator");
  ValveAddress a{"Valve 1_9 Set"};
  ValveAddress b{"312"};
  EXPECT_EQ(state(v.read(a)), ValveState::Closed);
  ASSERT_TRUE(v.open(a));
  EXPECT_EQ(state(v.read(a)), ValveState::Open);
  EXPECT_EQ(state(v.read(b)), ValveState::Closed);
  ASSERT_TRUE(v.close(a));
  EXPECT_EQ(state(v.read(a)), ValveState::Closed);
}

TEST(SimValvesRegistry, CreatesValveActuator) {
  const DriverRegistry& reg = DriverRegistry::global();
  ASSERT_TRUE(reg.contains("sim_valves"));
  TransportOptions o;
  o.name = "bus";
  auto t = SimTransport::scripted({}, o);
  toml::table options{{"kind", "sim_valves"}, {"transport", "bus"}};
  auto made = reg.create("sim_valves", *t, options, DriverContext{"switch_controller", nullptr});
  ASSERT_TRUE(made) << made.error().what;
  EXPECT_EQ((*made)->name(), "switch_controller");
  auto* actuator = capability<IValveActuator>(**made);
  ASSERT_NE(actuator, nullptr);
  ASSERT_TRUE(actuator->open(ValveAddress{"A"}));
  EXPECT_EQ(state(actuator->read(ValveAddress{"A"})), ValveState::Open);
}

}  // namespace
