// plc2000_gauges over a simulated Modbus PLC (plan 2026-10-05, task B7).
#include "pychron/devices/plc2000_gauges.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <map>

#include "pychron/devices/driver_registry.hpp"
#include "pychron/devices/modbus_device_sim.hpp"

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

// Holding registers of a PLC: floats stored in `order` at their registers.
struct Plc {
  std::map<std::uint16_t, std::uint16_t> regs;
  void put(std::uint16_t reg, float v, mb::WordOrder order = mb::WordOrder::CDAB) {
    auto w = mb::encode_float(v, order);
    regs[reg] = w[0];
    regs[reg + 1] = w[1];
  }
  ModbusDeviceSim device(std::uint8_t unit = 1) {
    ModbusDeviceSim d;
    d.unit = unit;
    d.read_holding = [this](std::uint16_t a) -> std::optional<std::uint16_t> {
      auto it = regs.find(a);
      return it == regs.end() ? std::nullopt : std::optional<std::uint16_t>(it->second);
    };
    return d;
  }
};

Result<std::unique_ptr<Device>> make(Transport& t, toml::table options) {
  return DriverRegistry::global().create("plc2000_gauges", t, options, DriverContext{"plc_gauges"});
}

toml::table with_channels(std::initializer_list<int> channels) {
  toml::array a;
  for (int c : channels) a.push_back(c);
  return toml::table{{"channels", a}};
}

}  // namespace

TEST(Plc2000Gauges, ChannelNReadsAFloatFromRegisterNMinusOne) {
  Plc plc;
  plc.put(0, 2.5e-8F);  // channel 1
  plc.put(2, 7.5e-3F);  // channel 3
  auto bus = SimTransport::hooked(plc.device().hook(), fast());
  ASSERT_TRUE(bus->open());
  auto dev = make(*bus, with_channels({1, 3}));
  ASSERT_TRUE(dev) << dev.error().what;
  auto* gauge = capability<IChannelPressureGauge>(**dev);
  EXPECT_FLOAT_EQ(static_cast<float>(*gauge->read_pressure(1)), 2.5e-8F);
  EXPECT_FLOAT_EQ(static_cast<float>(*gauge->read_pressure(3)), 7.5e-3F);
  // Read holding registers at 2, two of them.
  const auto last = mb::decode_request(bus->written().back());
  ASSERT_TRUE(last);
  EXPECT_EQ(last->function, 0x03);
  EXPECT_EQ(last->start, 2);
  EXPECT_EQ(last->count, 2);
  auto none = gauge->read_pressure(2);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
}

TEST(Plc2000Gauges, WordOrderUnitAndOffsetAreConfig) {
  Plc plc;
  plc.put(10, 1.25e-6F, mb::WordOrder::ABCD);
  auto bus = SimTransport::hooked(plc.device(/*unit=*/7).hook(), fast());
  ASSERT_TRUE(bus->open());
  auto options = with_channels({10});
  options.insert("unit", 7);
  options.insert("word_order", "ABCD");
  options.insert("register_offset", 0);
  auto dev = make(*bus, options);
  ASSERT_TRUE(dev) << dev.error().what;
  EXPECT_FLOAT_EQ(static_cast<float>(*capability<IPressureGauge>(**dev)->read_pressure()), 1.25e-6F);
}

TEST(Plc2000Gauges, NoPressureIsNeverAReading) {
  Plc plc;
  plc.put(0, -1.0F);
  plc.put(2, std::numeric_limits<float>::quiet_NaN());
  auto bus = SimTransport::hooked(plc.device().hook(), fast());
  ASSERT_TRUE(bus->open());
  auto dev = make(*bus, with_channels({1, 3, 9}));
  ASSERT_TRUE(dev);
  auto* gauge = capability<IChannelPressureGauge>(**dev);
  for (int ch : {1, 3}) {
    auto r = gauge->read_pressure(ch);
    ASSERT_FALSE(r) << ch;
    EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  }
  auto missing = gauge->read_pressure(9);  // the PLC has no register 8: exception 2
  ASSERT_FALSE(missing);
  EXPECT_NE(missing.error().what.find("illegal data address"), std::string::npos) << missing.error().what;
  EXPECT_EQ(missing.error().device, "plc_gauges");
}

TEST(Plc2000Gauges, AnotherUnitsReplyIsNotTaken) {
  Plc plc;
  plc.put(0, 1e-8F);
  auto bus = SimTransport::hooked(plc.device(/*unit=*/2).hook(), fast());
  ASSERT_TRUE(bus->open());
  auto dev = make(*bus, with_channels({1}));  // unit 1: nobody answers
  ASSERT_TRUE(dev);
  auto r = capability<IPressureGauge>(**dev)->read_pressure();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
}

TEST(Plc2000Gauges, ConfigIsChecked) {
  auto bus = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, fast());
  auto bad_order = with_channels({1});
  bad_order.insert("word_order", "little");
  EXPECT_FALSE(make(*bus, bad_order));
  auto bad_unit = with_channels({1});
  bad_unit.insert("unit", 300);
  EXPECT_FALSE(make(*bus, bad_unit));
  EXPECT_FALSE(make(*bus, with_channels({0})));
  EXPECT_FALSE(make(*bus, with_channels({1, 1})));
  EXPECT_FALSE(make(*bus, with_channels({})));
}
