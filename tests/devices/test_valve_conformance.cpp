// The IValveActuator conformance suite over the actuators that predate it
// (plan 2026-10-05, task A0). New actuators run it in their own tests.

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/devices/proxr_board_sim.hpp"
#include "pychron/devices/proxr_relay.hpp"
#include "pychron/devices/sim_valves.hpp"
#include "valve_conformance.hpp"

using namespace pychron;
using namespace pychron::test;
using namespace std::chrono_literals;

namespace {

TransportOptions fast(std::string name) {
  TransportOptions o;
  o.name = std::move(name);
  o.timeout = 50ms;
  return o;
}

struct SimValvesRig final : ValveRig {
  SimValves valves{"sim"};
  IValveActuator& actuator() override { return valves; }
  ValveAddress first() override { return {"A"}; }
  ValveAddress second() override { return {"B"}; }
};

struct ProxrRig final : ValveRig {
  ProxrBoardSim board;
  WireTap wire{board.hook(), Bytes{0x07}};
  std::unique_ptr<SimTransport> bus = SimTransport::hooked(wire.hook(), fast("valve_bus"));
  ProxrRelay relay{"relay", *bus};

  ProxrRig() { EXPECT_TRUE(bus->open()); }
  IValveActuator& actuator() override { return relay; }
  ValveAddress first() override { return {"3"}; }
  ValveAddress second() override { return {"12"}; }  // another bank
  std::optional<ValveAddress> bad() override { return ValveAddress{"256"}; }
  WireTap* tap() override { return &wire; }
};

}  // namespace

TEST(ValveConformance, SimValves) {
  SimValvesRig rig;
  expect_valve_conformance(rig);
}

TEST(ValveConformance, ProxrRelay) {
  ProxrRig rig;
  expect_valve_conformance(rig);
}
