// plc2000_heater over a simulated PLC (plan 2026-10-05, task E2).
#include "pychron/devices/plc2000_heater.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/devices/capabilities.hpp"
#include "pychron/devices/extraction/capability.hpp"

using namespace pychron;
using namespace std::chrono_literals;
namespace mb = pychron::codec::modbus;

namespace {

TransportOptions fast() {
  TransportOptions o;
  o.name = "plc";
  o.timeout = 50ms;
  return o;
}

// AELAMS-like addresses: coils 1 and 2, registers at 11 and 21 (1-based).
toml::table aelams() {
  return toml::table{{"enable", 2}, {"use_pid", 1}, {"setpoint", 11}, {"readback", 21}};
}

struct Rig {
  explicit Rig(toml::table options = aelams()) {
    auto parsed = Plc2000Heater::parse_options(options);
    EXPECT_TRUE(parsed) << parsed.error().what;
    sim = std::make_unique<Plc2000HeaterSim>(clock, *parsed, 25.0, 60s);
    bus = SimTransport::hooked(sim->hook(), fast());
    EXPECT_TRUE(bus->open());
    auto made = DriverRegistry::global().create("plc2000_heater", *bus, options, DriverContext{"heater"});
    EXPECT_TRUE(made) << made.error().what;
    device = std::move(*made);
    heater = capability<IHeater>(*device);
  }

  std::optional<mb::Request> last() const {
    auto r = mb::decode_request(bus->written().back());
    return r ? std::optional<mb::Request>(*r) : std::nullopt;
  }

  ManualClock clock;
  std::unique_ptr<Plc2000HeaterSim> sim;
  std::unique_ptr<SimTransport> bus;
  std::unique_ptr<Device> device;
  IHeater* heater = nullptr;
};

}  // namespace

TEST(Plc2000Heater, CoilsAreOneBasedAddressesLessOne) {
  Rig r;
  ASSERT_NE(r.heater, nullptr);
  EXPECT_EQ(*r.heater->enabled(), false);
  EXPECT_EQ(r.last()->function, 0x01);
  EXPECT_EQ(r.last()->start, 1);  // enable = 2
  ASSERT_TRUE(r.heater->set_enabled(true));
  EXPECT_EQ(r.last()->function, 0x05);
  EXPECT_EQ(r.last()->start, 1);
  EXPECT_TRUE(r.sim->enabled());
  EXPECT_EQ(*r.heater->enabled(), true);
  EXPECT_FALSE(r.sim->use_pid());
}

TEST(Plc2000Heater, UsePidCoilAtAddressOneIsRead) {
  // Legacy tested `if self.use_pid_address:` after subtracting one, so a
  // use_pid coil at 1 was never read.
  Rig r;
  ASSERT_TRUE(r.heater->set_use_pid(true));
  EXPECT_EQ(r.last()->start, 0);
  auto on = r.heater->use_pid();
  ASSERT_TRUE(on) << on.error().what;
  EXPECT_TRUE(*on);
  EXPECT_EQ(r.last()->function, 0x01);
  EXPECT_EQ(r.last()->start, 0);
}

TEST(Plc2000Heater, SetpointIsWrittenAsInt32ToHoldingAndReadAsFloatFromInput) {
  Rig r;
  ASSERT_TRUE(r.heater->set_setpoint(450.0));
  auto write = r.last();
  ASSERT_TRUE(write);
  EXPECT_EQ(write->function, 0x10);
  EXPECT_EQ(write->start, 10);
  ASSERT_EQ(write->values.size(), 2u);
  EXPECT_EQ(mb::decode_int32(write->values[0], write->values[1], mb::WordOrder::CDAB), 450);
  EXPECT_DOUBLE_EQ(r.sim->setpoint(), 450.0);
  EXPECT_DOUBLE_EQ(*r.heater->setpoint(), 450.0);
  EXPECT_EQ(r.last()->function, 0x04);
  EXPECT_EQ(r.last()->start, 10);
  EXPECT_EQ(r.last()->count, 2);
}

TEST(Plc2000Heater, AFractionalSetpointIsRefusedNotTruncated) {
  Rig r;
  const auto sent = r.bus->written().size();
  auto bad = r.heater->set_setpoint(450.5);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.bus->written().size(), sent);
}

TEST(Plc2000Heater, Float32WriteFormatTakesFractions) {
  auto options = aelams();
  options.insert("setpoint_write_format", "float32");
  Rig r(options);
  ASSERT_TRUE(r.heater->set_setpoint(450.5));
  auto write = r.last();
  EXPECT_FLOAT_EQ(mb::decode_float(write->values[0], write->values[1], mb::WordOrder::CDAB), 450.5F);
  EXPECT_DOUBLE_EQ(*r.heater->setpoint(), 450.5);
}

TEST(Plc2000Heater, ReadbackFollowsTheSetpointWhileOnAndDecaysWhenOff) {
  Rig r;
  EXPECT_NEAR(*r.heater->readback(), 25.0, 1e-4);
  ASSERT_TRUE(r.heater->set_setpoint(400.0));
  r.clock.advance(10min);
  EXPECT_NEAR(*r.heater->readback(), 25.0, 1e-4);  // off: nothing happens
  ASSERT_TRUE(r.heater->set_enabled(true));
  r.clock.advance(60s);
  const double one_tau = *r.heater->readback();
  EXPECT_NEAR(one_tau, 25.0 + 375.0 * (1 - std::exp(-1.0)), 0.01);
  r.clock.advance(10min);
  EXPECT_NEAR(*r.heater->readback(), 400.0, 0.1);
  ASSERT_TRUE(r.heater->set_enabled(false));
  r.clock.advance(10min);
  EXPECT_NEAR(*r.heater->readback(), 25.0, 0.1);
}

TEST(Plc2000Heater, AMissingAddressIsNotSupported) {
  Rig r(toml::table{{"enable", 1}, {"readback", 3}});
  const auto sent = r.bus->written().size();
  for (auto e : {r.heater->use_pid().error(), r.heater->set_use_pid(true).error(), r.heater->setpoint().error(),
                 r.heater->set_setpoint(1.0).error()}) {
    EXPECT_TRUE(extraction::is_not_supported(e)) << e.what;
  }
  EXPECT_EQ(r.bus->written().size(), sent);
  EXPECT_TRUE(r.heater->readback());
}

TEST(Plc2000Heater, WordOrderAndUnitAreConfig) {
  auto options = aelams();
  options.insert("word_order", "abcd");
  options.insert("unit", 5);
  Rig r(options);
  ASSERT_TRUE(r.heater->set_setpoint(300.0));
  auto write = r.last();
  EXPECT_EQ(write->unit, 5);
  EXPECT_EQ(mb::decode_int32(write->values[0], write->values[1], mb::WordOrder::ABCD), 300);
  EXPECT_DOUBLE_EQ(*r.heater->setpoint(), 300.0);
}

TEST(Plc2000Heater, SilenceAndExceptionsAreErrors) {
  Rig r;
  // Another unit's PLC: nobody answers.
  auto other = SimTransport::hooked(r.sim->hook(), fast());
  ASSERT_TRUE(other->open());
  auto options = aelams();
  options.insert("unit", 9);
  auto dev = DriverRegistry::global().create("plc2000_heater", *other, options, DriverContext{"heater"});
  ASSERT_TRUE(dev);
  auto silent = capability<IHeater>(**dev)->enabled();
  ASSERT_FALSE(silent);
  EXPECT_EQ(silent.error().kind, ErrorKind::Timeout);
  // A coil the PLC does not have: exception 2.
  auto wrong = toml::table{{"enable", 7}};
  auto bus = SimTransport::hooked(r.sim->hook(), fast());
  ASSERT_TRUE(bus->open());
  auto dev2 = DriverRegistry::global().create("plc2000_heater", *bus, wrong, DriverContext{"heater"});
  ASSERT_TRUE(dev2);
  auto missing = capability<IHeater>(**dev2)->enabled();
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Protocol);
  EXPECT_NE(missing.error().what.find("illegal data address"), std::string::npos);
}

TEST(Plc2000Heater, ConfigIsChecked) {
  auto bus = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, fast());
  auto make = [&](toml::table t) { return DriverRegistry::global().create("plc2000_heater", *bus, t, {}); };
  EXPECT_FALSE(make(toml::table{{"enable", 0}}));  // 1-based
  EXPECT_FALSE(make(toml::table{{"setpoint", 65536}}));
  EXPECT_FALSE(make(toml::table{{"unit", 256}}));
  EXPECT_FALSE(make(toml::table{{"word_order", "little"}}));
  EXPECT_FALSE(make(toml::table{{"setpoint_write_format", "int16"}}));
  EXPECT_TRUE(make(toml::table{}));  // nothing supported, but valid
}
