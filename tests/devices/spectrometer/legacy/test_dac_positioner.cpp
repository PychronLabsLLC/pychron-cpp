#include "pychron/devices/spectrometer/legacy/dac_positioner.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "pychron/codecs/map215.hpp"
#include "pychron/devices/driver_registry.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

namespace {

Result<std::unique_ptr<DacPositioner>> make(Transport& t, std::string_view options) {
  const auto table = table_of(options);
  return DacPositioner::create(DriverArgs{"magnet_dac", t, table});
}

}  // namespace

TEST(DacPositioner, SetSelectsRangeThenWritesCode) {
  auto sim = open_scripted({write_only("B3."), write_only("W32768.")});
  auto dac = make(*sim, "range = 3");
  ASSERT_TRUE(dac) << to_string(dac.error());

  ASSERT_TRUE((*dac)->set(5.0));
  expect_verified(*sim);
}

TEST(DacPositioner, ReadIsCachedQuantizedSetpointWithoutBusTraffic) {
  auto sim = open_scripted({write_only("B0."), write_only("W6554.")});
  auto dac = make(*sim, "");
  ASSERT_TRUE(dac);
  ASSERT_TRUE((*dac)->set(1.0));
  const auto before = sim->written().size();

  auto v = (*dac)->read();
  ASSERT_TRUE(v);
  EXPECT_DOUBLE_EQ(*v, codec::map215::to_volts(6554, 10.0));
  EXPECT_EQ(sim->written().size(), before);
  EXPECT_EQ((*dac)->native_axis(), IMassPositioner::Axis::Dac);
}

TEST(DacPositioner, PowersUpReportingLimitNearestZero) {
  auto sim = open_scripted({});
  auto dac = make(*sim, "min = 2.0\nmax = 8.0");
  ASSERT_TRUE(dac);
  EXPECT_EQ((*dac)->limits(), (Limits{2.0, 8.0}));
  EXPECT_DOUBLE_EQ(*(*dac)->read(), 2.0);
}

TEST(DacPositioner, OutsideLimitsIsConfigAndWritesNothing) {
  auto sim = open_scripted({});
  auto dac = make(*sim, "max = 5.0");
  ASSERT_TRUE(dac);
  auto r = (*dac)->set(6.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_TRUE(sim->written().empty());
}

TEST(DacPositioner, FailedWriteKeepsPreviousCache) {
  // Script expects a different code: the second write is rejected by the sim.
  auto sim = open_scripted({write_only("B0."), write_only("W6554."), write_only("B0."), write_only("W0.")});
  auto dac = make(*sim, "");
  ASSERT_TRUE(dac);
  ASSERT_TRUE((*dac)->set(1.0));
  auto r = (*dac)->set(9.0);
  ASSERT_FALSE(r);
  EXPECT_DOUBLE_EQ(*(*dac)->read(), codec::map215::to_volts(6554, 10.0));
}

TEST(DacPositioner, CreateRejectsBadOptions) {
  auto sim = open_scripted({});
  for (const char* options : {"protocol = \"labjack\"", "channel = 1", "range = 10", "full_scale = 0.0",
                              "min = 5.0\nmax = 5.0", "max = 20.0", "min = -1.0"}) {
    auto dac = make(*sim, options);
    ASSERT_FALSE(dac) << options;
    EXPECT_EQ(dac.error().kind, ErrorKind::Config) << options;
  }
}

TEST(DacPositioner, RegisteredAsDacPositioner) {
  auto sim = open_scripted({});
  auto dev = DriverRegistry::global().create("dac_positioner", *sim, table_of("roles = [\"positioner\"]"),
                                             DriverContext{"magnet_dac"});
  ASSERT_TRUE(dev) << to_string(dev.error());
  EXPECT_NE(dynamic_cast<IMassPositioner*>(dev->get()), nullptr);
}

TEST(Map215SimHook, ReportsOutputOnSelectedRange) {
  std::vector<std::pair<int, double>> outputs;
  auto sim = open_hooked(map215_sim_hook({10.0, [&](int range, double v) { outputs.emplace_back(range, v); }}));
  Map215Dac dac(*sim, 2, 10.0);
  auto v = dac.write(2.5);
  ASSERT_TRUE(v) << to_string(v.error());
  ASSERT_EQ(outputs.size(), 1U);
  EXPECT_EQ(outputs[0].first, 2);
  EXPECT_DOUBLE_EQ(outputs[0].second, *v);
  EXPECT_NEAR(*v, 2.5, 10.0 / 65535);
}
