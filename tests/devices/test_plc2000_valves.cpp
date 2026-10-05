// plc2000_valves over a simulated PLC coil bank (plan 2026-10-05, task A7).
#include "pychron/devices/plc2000_valves.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <map>

#include "pychron/codecs/modbus.hpp"
#include "pychron/devices/modbus_device_sim.hpp"
#include "valve_conformance.hpp"

using namespace pychron;
using namespace pychron::test;
using namespace std::chrono_literals;
namespace mb = pychron::codec::modbus;

namespace {

TransportOptions fast() {
  TransportOptions o;
  o.name = "plc";
  o.timeout = 50ms;
  return o;
}

// The PLC's coils 0..15; anything else is an illegal address.
struct CoilBank {
  std::map<std::uint16_t, bool> coils;
  bool echo_wrong_value = false;

  CoilBank() {
    for (std::uint16_t c = 0; c < 16; ++c) coils[c] = false;
  }
  ModbusDeviceSim device(std::uint8_t unit = 1) {
    ModbusDeviceSim d;
    d.unit = unit;
    d.read_coil = [this](std::uint16_t a) -> std::optional<bool> {
      auto it = coils.find(a);
      return it == coils.end() ? std::nullopt : std::optional<bool>(it->second);
    };
    d.write_coil = [this](std::uint16_t a, bool on) {
      auto it = coils.find(a);
      if (it == coils.end()) return false;
      it->second = on;
      return true;
    };
    return d;
  }
  SimTransport::Hook hook() {
    auto inner = device().hook();
    return [this, inner](const Bytes& tx) {
      Bytes reply = inner(tx);
      // A function-05 echo has the value at bytes 10..11.
      if (echo_wrong_value && reply.size() == 12 && reply[7] == 0x05) reply[10] ^= 0xFF;
      return reply;
    };
  }
};

// A reply for another transaction: decodes as a Protocol error.
Bytes stray_reply() { return Bytes{0xFF, 0xFE, 0x00, 0x00, 0x00, 0x03, 0x01, 0x81, 0x02}; }

struct Rig final : ValveRig {
  explicit Rig(Plc2000ValvesOptions options = {}) : driver("plc_valves", *bus, options) { EXPECT_TRUE(bus->open()); }

  CoilBank bank;
  WireTap wire{bank.hook(), stray_reply()};
  std::unique_ptr<SimTransport> bus = SimTransport::hooked(wire.hook(), fast());
  Plc2000Valves driver;

  IValveActuator& actuator() override { return driver; }
  ValveAddress first() override { return {"1"}; }
  ValveAddress second() override { return {"16"}; }
  std::optional<ValveAddress> bad() override { return ValveAddress{"Valve 1"}; }
  WireTap* tap() override { return &wire; }

  std::vector<mb::Request> requests() const {
    std::vector<mb::Request> out;
    for (const auto& tx : bus->written())
      if (auto r = mb::decode_request(tx)) out.push_back(*r);
    return out;
  }
};

}  // namespace

TEST(Plc2000Valves, PassesTheValveConformanceSuite) {
  Rig rig;
  expect_valve_conformance(rig);
}

TEST(Plc2000Valves, AValveIsItsCoilLessOne) {
  Rig rig;
  ASSERT_TRUE(rig.driver.open({"5"}));
  EXPECT_TRUE(rig.bank.coils[4]);
  auto write = rig.requests().back();
  EXPECT_EQ(write.function, 0x05);
  EXPECT_EQ(write.start, 4);
  EXPECT_TRUE(write.coil);
  EXPECT_EQ(*rig.driver.read({"5"}), ValveState::Open);
  auto read = rig.requests().back();
  EXPECT_EQ(read.function, 0x01);
  EXPECT_EQ(read.start, 4);
  EXPECT_EQ(read.count, 1);
  ASSERT_TRUE(rig.driver.close({"5"}));
  EXPECT_FALSE(rig.bank.coils[4]);
}

TEST(Plc2000Valves, CoilOffsetAndUnitAreConfig) {
  Rig rig(Plc2000ValvesOptions{1, 0});  // unit 1, 0-based addresses
  ASSERT_TRUE(rig.driver.open({"5"}));
  EXPECT_TRUE(rig.bank.coils[5]);
  auto bus = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, fast());
  auto make = [&](toml::table t) { return DriverRegistry::global().create("plc2000_valves", *bus, t, {}); };
  EXPECT_TRUE(make(toml::table{{"unit", 3}, {"coil_offset", 0}}));
  EXPECT_FALSE(make(toml::table{{"unit", 256}}));
  EXPECT_FALSE(make(toml::table{{"coil_offset", 70000}}));
}

TEST(Plc2000Valves, AnAddressOffTheCoilRangeIsConfig) {
  Rig rig;
  for (const char* bad : {"0", "65537", "-1", "x"}) {
    auto r = rig.driver.open({bad});
    ASSERT_FALSE(r) << bad;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << bad;
  }
  EXPECT_TRUE(rig.bus->written().empty());
}

TEST(Plc2000Valves, AnEchoThatDiffersIsAProtocolError) {
  // Legacy took any reply to a write as success.
  Rig rig;
  rig.bank.echo_wrong_value = true;
  auto r = rig.driver.open({"2"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(Plc2000Valves, AnExceptionIsAProtocolErrorNotAState) {
  Rig rig;
  auto r = rig.driver.read({"40"});  // the PLC has 16 coils
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("illegal data address"), std::string::npos) << r.error().what;
}

TEST(Plc2000Valves, ReadManyReadsEachRunOfCoilsOnce) {
  Rig rig;
  rig.bank.coils[1] = true;   // valve 2
  rig.bank.coils[10] = true;  // valve 11
  const auto before = rig.bus->written().size();
  auto states = rig.driver.read_many({{"3"}, {"2"}, {"1"}, {"11"}, {"12"}, {"x"}, {"2"}});
  ASSERT_EQ(states.size(), 7u);
  EXPECT_EQ(*states[0], ValveState::Closed);
  EXPECT_EQ(*states[1], ValveState::Open);
  EXPECT_EQ(*states[2], ValveState::Closed);
  EXPECT_EQ(*states[3], ValveState::Open);
  EXPECT_EQ(*states[4], ValveState::Closed);
  ASSERT_FALSE(states[5]);
  EXPECT_EQ(states[5].error().kind, ErrorKind::Config);
  EXPECT_EQ(*states[6], ValveState::Open);
  // Coils 0-2 and 10-11: two requests, not six.
  const auto requests = rig.requests();
  ASSERT_EQ(requests.size() - before, 2u);
  EXPECT_EQ(requests[before].start, 0);
  EXPECT_EQ(requests[before].count, 3);
  EXPECT_EQ(requests[before + 1].start, 10);
  EXPECT_EQ(requests[before + 1].count, 2);
}

TEST(Plc2000Valves, AFailedRunFailsOnlyItsValves) {
  Rig rig;
  auto states = rig.driver.read_many({{"1"}, {"40"}});  // coil 39 is not there
  ASSERT_EQ(states.size(), 2u);
  EXPECT_EQ(*states[0], ValveState::Closed);
  ASSERT_FALSE(states[1]);
  EXPECT_EQ(states[1].error().kind, ErrorKind::Protocol);
}
