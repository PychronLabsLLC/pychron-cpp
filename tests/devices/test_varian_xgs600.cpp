// varian_xgs600 over its simulated controller (plan 2026-10-05, task B1).
#include "pychron/devices/varian_xgs600.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/devices/driver_registry.hpp"

using namespace pychron;
using namespace std::chrono_literals;

namespace {

TransportOptions fast() {
  TransportOptions o;
  o.name = "xgs_bus";
  o.timeout = 50ms;
  return o;
}

Result<std::unique_ptr<Device>> make(Transport& t, toml::table options) {
  return DriverRegistry::global().create("varian_xgs600", t, options, DriverContext{"xgs"});
}

toml::table labelled(std::initializer_list<const char*> labels) {
  toml::array a;
  for (auto l : labels) a.push_back(l);
  return toml::table{{"labels", a}};
}

}  // namespace

TEST(VarianXgs600, ChannelNReadsTheNthLabel) {
  Xgs600SimModel model;
  model.pressure = [](const std::string& label) -> std::optional<double> {
    if (label == "CNV1") return 1.5e-3;
    if (label == "IMG1") return 2.5e-9;
    return std::nullopt;
  };
  auto bus = SimTransport::hooked(xgs600_sim_hook(model), fast());
  ASSERT_TRUE(bus->open());
  auto dev = make(*bus, labelled({"CNV1", "IMG1", "HFIG1"}));
  ASSERT_TRUE(dev) << dev.error().what;
  auto* gauge = capability<IChannelPressureGauge>(**dev);
  ASSERT_NE(gauge, nullptr);
  EXPECT_EQ(gauge->pressure_channels(), (std::vector<int>{1, 2, 3}));
  EXPECT_DOUBLE_EQ(*gauge->read_pressure(1), 1.5e-3);
  EXPECT_DOUBLE_EQ(*gauge->read_pressure(2), 2.5e-9);
  EXPECT_EQ(to_string(bus->written().back()), "#0002UIMG1\r");

  auto off = gauge->read_pressure(3);  // the sim reports HFIG1 off
  ASSERT_FALSE(off);
  EXPECT_EQ(off.error().kind, ErrorKind::Protocol);
  EXPECT_NE(off.error().what.find("gauge off"), std::string::npos);
  EXPECT_EQ(off.error().device, "xgs");

  auto none = gauge->read_pressure(4);
  ASSERT_FALSE(none);
  EXPECT_EQ(none.error().kind, ErrorKind::Config);
}

TEST(VarianXgs600, ControllersShareABusByAddress) {
  Xgs600SimModel a, b;
  a.address = "01";
  a.pressure = [](const std::string&) -> std::optional<double> { return 1e-8; };
  b.address = "02";
  b.pressure = [](const std::string&) -> std::optional<double> { return 2e-8; };
  auto ha = xgs600_sim_hook(a), hb = xgs600_sim_hook(b);
  auto bus = SimTransport::hooked([&](const Bytes& tx) {
    Bytes r = ha(tx);
    return r.empty() ? hb(tx) : r;
  }, fast());
  ASSERT_TRUE(bus->open());
  auto opts = labelled({"IG1"});
  opts.insert("address", "02");
  auto dev = make(*bus, opts);
  ASSERT_TRUE(dev) << dev.error().what;
  EXPECT_DOUBLE_EQ(*capability<IPressureGauge>(**dev)->read_pressure(), 2e-8);
}

TEST(VarianXgs600, ConfigIsChecked) {
  auto bus = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, fast());
  EXPECT_FALSE(make(*bus, toml::table{}));                  // labels required
  EXPECT_FALSE(make(*bus, labelled({})));                    // at least one
  EXPECT_FALSE(make(*bus, labelled({"IG1", "IG1"})));        // unique
  EXPECT_FALSE(make(*bus, labelled({"IG 1"})));              // a label is letters and digits
  auto bad = labelled({"IG1"});
  bad.insert("address", "0");
  EXPECT_FALSE(make(*bus, bad));
}

TEST(VarianXgs600, SilenceIsATimeoutNotAReading) {
  auto bus = SimTransport::hooked([](const Bytes&) { return Bytes{}; }, fast());
  ASSERT_TRUE(bus->open());
  auto dev = make(*bus, labelled({"IG1"}));
  ASSERT_TRUE(dev);
  auto r = capability<IPressureGauge>(**dev)->read_pressure();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
}
