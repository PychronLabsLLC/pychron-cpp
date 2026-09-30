#include "pychron/devices/spectrometer/legacy/serial_hv.hpp"

#include <gtest/gtest.h>

#include "pychron/devices/driver_registry.hpp"
#include "spectrometer/legacy/sim_util.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace legacy_test;

TEST(SerialHv, SetHvSendsVsetAndExpectsOk) {
  auto sim = open_scripted({step("VSET 4500.0\r", "OK\r")});
  SerialHv hv("spellman", *sim);
  auto r = hv.set_hv(4500.0);
  ASSERT_TRUE(r) << to_string(r.error());
  expect_verified(*sim);
}

TEST(SerialHv, ReadHvIsMeasuredOutput) {
  auto sim = open_scripted({step("VOUT?\r", "4499.8\r")});
  SerialHv hv("spellman", *sim);
  auto v = hv.read_hv();
  ASSERT_TRUE(v);
  EXPECT_DOUBLE_EQ(*v, 4499.8);
}

TEST(SerialHv, ReadParamReportsSetpointAndActual) {
  auto sim = open_scripted({step("VSET?\r", "4500.0\r"), step("VOUT?\r", "4498.5\r")});
  SerialHv hv("spellman", *sim);
  auto rb = hv.read_param(ParamId{SourceParam::HV});
  ASSERT_TRUE(rb) << to_string(rb.error());
  EXPECT_EQ(*rb, (Readback{4500.0, 4498.5}));
}

TEST(SerialHv, SupplyRejectionIsProtocolError) {
  auto sim = open_scripted({step("VSET 100.0\r", "ERR INTERLOCK\r")});
  SerialHv hv("spellman", *sim);
  auto r = hv.set_hv(100.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_EQ(r.error().device, "spellman");
}

TEST(SerialHv, OutOfRangeSetpointIsConfigWithoutTraffic) {
  auto sim = open_scripted({});
  SerialHv hv("spellman", *sim, 5000.0);
  for (double v : {-1.0, 5000.1}) {
    auto r = hv.set_hv(v);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
  }
  EXPECT_TRUE(sim->written().empty());
}

TEST(SerialHv, AdvertisesHvOnly) {
  auto sim = open_scripted({});
  SerialHv hv("spellman", *sim, 6000.0);
  ASSERT_EQ(hv.params().size(), 1U);
  EXPECT_EQ(hv.params()[0].id, ParamId{SourceParam::HV});
  EXPECT_EQ(hv.params()[0].range, (Range{0.0, 6000.0}));
  auto r = hv.set_param(ParamId{SourceParam::TrapCurrent}, 1.0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(SerialHv, CreateValidatesMaxHv) {
  auto sim = open_scripted({});
  const auto bad = table_of("max_hv = -5.0");
  auto r = SerialHv::create(DriverArgs{"hv", *sim, bad});
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  auto dev = DriverRegistry::global().create("serial_hv", *sim, table_of("max_hv = 6000.0"), DriverContext{"hv"});
  ASSERT_TRUE(dev) << to_string(dev.error());
  EXPECT_NE(dynamic_cast<IBeamSource*>(dev->get()), nullptr);
}

TEST(HvSimHook, OutputFollowsModelAndRangeIsEnforced) {
  double seen = -1.0;
  auto sim = open_hooked(hv_sim_hook({0.0, 5000.0, [&](double s) { seen = s; }, [](double s) { return s - 1.0; }}));
  SerialHv hv("spellman", *sim, 10000.0);
  ASSERT_TRUE(hv.set_hv(3000.0));
  EXPECT_DOUBLE_EQ(seen, 3000.0);
  EXPECT_DOUBLE_EQ(*hv.read_hv(), 2999.0);
  EXPECT_EQ(*hv.read_param(ParamId{SourceParam::HV}), (Readback{3000.0, 2999.0}));
  auto r = hv.set_hv(6000.0);  // driver allows it; the supply refuses
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}
