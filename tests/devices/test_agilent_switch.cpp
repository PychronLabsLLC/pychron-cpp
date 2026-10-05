// agilent_switch over the simulated 34970A (plan 2026-10-05, task A1).

#include "pychron/devices/agilent_switch.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/devices/agilent_unit_sim.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "valve_conformance.hpp"

using namespace pychron;
using namespace pychron::test;
using namespace std::chrono_literals;

namespace {

TransportOptions fast() {
  TransportOptions o;
  o.name = "agilent_bus";
  o.timeout = 50ms;
  return o;
}

struct Rig final : ValveRig {
  explicit Rig(bool invert = false) : driver("switch_controller", *bus, invert) { EXPECT_TRUE(bus->open()); }

  AgilentUnitSim unit;
  WireTap wire{unit.hook(), to_bytes("??\n")};
  std::unique_ptr<SimTransport> bus = SimTransport::hooked(wire.hook(), fast());
  AgilentSwitch driver;

  IValveActuator& actuator() override { return driver; }
  ValveAddress first() override { return {"101"}; }
  ValveAddress second() override { return {"312"}; }
  std::optional<ValveAddress> bad() override { return ValveAddress{"401"}; }
  WireTap* tap() override { return &wire; }

  std::vector<std::string> sent() const {
    std::vector<std::string> out;
    for (const auto& tx : bus->written()) out.push_back(to_string(tx));
    return out;
  }
};

}  // namespace

TEST(AgilentSwitch, PassesTheValveConformanceSuite) {
  Rig rig;
  expect_valve_conformance(rig);
}

TEST(AgilentSwitch, PassesItInvertedToo) {
  Rig rig(/*invert=*/true);
  expect_valve_conformance(rig);
}

TEST(AgilentSwitch, OpeningAValveOpensItsRelayAndAsksTheErrorQueue) {
  Rig rig;
  EXPECT_EQ(*rig.driver.read(ValveAddress{"101"}), ValveState::Closed);  // relays start closed
  ASSERT_TRUE(rig.driver.close(ValveAddress{"101"}));
  EXPECT_TRUE(rig.unit.relay_closed("101"));
  ASSERT_TRUE(rig.driver.open(ValveAddress{"101"}));
  EXPECT_FALSE(rig.unit.relay_closed("101"));
  auto sent = rig.sent();
  ASSERT_GE(sent.size(), 2u);
  EXPECT_EQ(std::vector<std::string>(sent.end() - 2, sent.end()),
            (std::vector<std::string>{"ROUT:OPEN (@101)\n", "SYST:ERR?\n"}));
  ASSERT_TRUE(rig.driver.read(ValveAddress{"101"}));
  EXPECT_EQ(rig.sent().back(), "ROUT:OPEN? (@101)\n");
}

TEST(AgilentSwitch, InvertSwapsCommandAndQuery) {
  Rig rig(/*invert=*/true);
  ASSERT_TRUE(rig.driver.open(ValveAddress{"101"}));
  EXPECT_TRUE(rig.unit.relay_closed("101"));  // an open valve is a closed relay
  auto sent = rig.sent();
  EXPECT_EQ(sent[sent.size() - 2], "ROUT:CLOSE (@101)\n");
  EXPECT_EQ(*rig.driver.read(ValveAddress{"101"}), ValveState::Open);
  EXPECT_EQ(rig.sent().back(), "ROUT:CLOSE? (@101)\n");
  ASSERT_TRUE(rig.driver.close(ValveAddress{"101"}));
  EXPECT_FALSE(rig.unit.relay_closed("101"));
  EXPECT_EQ(*rig.driver.read(ValveAddress{"101"}), ValveState::Closed);
}

TEST(AgilentSwitch, AnInstrumentErrorFailsTheCommandAndTheQueueIsDrained) {
  Rig rig;
  rig.unit.push_error(-221, "Settings conflict");
  rig.unit.push_error(-350, "Queue overflow");
  auto r = rig.driver.open(ValveAddress{"101"});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("-221 \"Settings conflict\" after ROUT:OPEN (@101)"), std::string::npos)
      << r.error().what;
  EXPECT_EQ(r.error().device, "switch_controller");
  EXPECT_EQ(rig.unit.queued_errors(), 0u);
  EXPECT_TRUE(rig.driver.open(ValveAddress{"101"}));  // clean again
}

TEST(AgilentSwitch, ErrorQueueDrainIsBounded) {
  Rig rig;
  for (int i = 0; i < 15; ++i) rig.unit.push_error(-100, "Command error");
  auto r = rig.driver.close(ValveAddress{"101"});
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("still not empty after 10 reads"), std::string::npos) << r.error().what;
  std::size_t asks = 0;
  for (const auto& s : rig.sent()) asks += s == "SYST:ERR?\n";
  EXPECT_EQ(asks, 10u);
}

TEST(AgilentSwitch, ConnectChecksTheIdentityAndNeverSelfTests) {
  Rig rig;
  rig.unit.push_error(-113, "Undefined header");  // left over from before
  ASSERT_TRUE(rig.driver.connect());
  EXPECT_EQ(rig.unit.queued_errors(), 0u);
  auto sent = rig.sent();
  ASSERT_GE(sent.size(), 3u);
  EXPECT_EQ(sent[0], "*IDN?\n");
  EXPECT_EQ(sent[1], "*CLS\n");
  for (const auto& s : sent) EXPECT_EQ(s.find("*TST"), std::string::npos);

  rig.unit.set_identity("LSCI,MODEL335,1234567/1234567,1.0");
  auto wrong = rig.driver.connect();
  ASSERT_FALSE(wrong);
  EXPECT_EQ(wrong.error().kind, ErrorKind::Config);
  EXPECT_NE(wrong.error().what.find("LSCI,MODEL335"), std::string::npos) << wrong.error().what;
}

TEST(AgilentSwitch, AChannelNotFittedIsTheInstrumentsError) {
  Rig rig;
  auto r = rig.driver.open(ValveAddress{"121"});  // valid form, no such channel on a 34903A
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("-222"), std::string::npos) << r.error().what;
}

TEST(AgilentSwitch, RegistryBuildsItWithInvert) {
  AgilentUnitSim unit;
  auto bus = SimTransport::hooked(unit.hook(), fast());
  ASSERT_TRUE(bus->open());
  toml::table options{{"invert", true}};
  auto dev = DriverRegistry::global().create("agilent_switch", *bus, options, DriverContext{"switch_controller"});
  ASSERT_TRUE(dev) << dev.error().what;
  auto* sw = dynamic_cast<AgilentSwitch*>(dev->get());
  ASSERT_NE(sw, nullptr);
  EXPECT_TRUE(sw->invert());
  toml::table wrong{{"invert", "yes"}};
  EXPECT_FALSE(DriverRegistry::global().create("agilent_switch", *bus, wrong, DriverContext{"x"}));
}
