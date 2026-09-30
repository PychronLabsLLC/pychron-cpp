#include "pychron/sim/sim_extraction_device.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

// Shared with tests/devices: the suites every extraction device must pass.
#include "../devices/extraction/conformance.hpp"

namespace pychron::sim {
namespace {

using namespace std::chrono_literals;
using extraction::Capability;
using extraction::CapabilitySet;
using extraction::ExtractUnits;

SimExtractionSettings laser_settings() {
  SimExtractionSettings s;
  s.name = "sim_co2";
  s.features = CapabilitySet{Capability::Laser, Capability::Stage, Capability::Pattern};
  s.max_power_watts = 50.0;
  s.ambient_c = 20.0;
  s.c_per_watt = 30.0;
  s.thermal_tau = 1s;
  s.release_c50 = 700.0;
  s.release_width_c = 50.0;
  s.gas_per_position = 2.0;
  s.positions = {"1", "2", "3"};
  s.move_time = 500ms;
  s.pattern_time = 2s;
  return s;
}

SimExtractionSettings furnace_settings() {
  SimExtractionSettings s = laser_settings();
  s.name = "sim_furnace";
  s.features = CapabilitySet{Capability::Furnace};
  return s;
}

struct Recorder {
  std::vector<GasRelease> releases;
  double total() const {
    double t = 0;
    for (auto& r : releases) t += r.amount;
    return t;
  }
};

class SimExtractionDeviceTest : public ::testing::Test {
 protected:
  ManualClock clock;
  SimExtractionDevice laser{laser_settings(), clock};
  Recorder rec;
  void SetUp() override {
    laser.on_gas_release([this](const GasRelease& r) { rec.releases.push_back(r); });
  }
};

TEST_F(SimExtractionDeviceTest, ExposesConfiguredFeaturesOnly) {
  EXPECT_EQ(extraction::capabilities(laser),
            (CapabilitySet{Capability::Laser, Capability::Stage, Capability::Pattern}));
  EXPECT_EQ(laser.furnace(), nullptr);
  EXPECT_FALSE(laser.supports(ExtractUnits::Celsius));
  EXPECT_TRUE(laser.supports(ExtractUnits::Percent));
  EXPECT_TRUE(laser.supports(ExtractUnits::Watts));
}

TEST_F(SimExtractionDeviceTest, SitsAtAmbientUntilFired) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(50.0, ExtractUnits::Percent));
  clock.advance(10s);
  EXPECT_NEAR(laser.temperature(), 20.0, 1e-9);  // not firing yet
}

TEST_F(SimExtractionDeviceTest, PowerDrivesTemperatureWithFirstOrderLag) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.move_to_position("1", false));
  ASSERT_TRUE(laser.extract(50.0, ExtractUnits::Percent));  // 25 W -> 20 + 750 C
  ASSERT_TRUE(laser.fire_laser());
  clock.advance(1s);  // one tau
  EXPECT_NEAR(laser.temperature(), 770.0 - 750.0 * std::exp(-1.0), 1e-6);
  clock.advance(20s);
  EXPECT_NEAR(laser.temperature(), 770.0, 1e-3);
  ASSERT_TRUE(laser.extract(10.0, ExtractUnits::Watts));  // 20 + 300 C
  clock.advance(20s);
  EXPECT_NEAR(laser.temperature(), 320.0, 1e-3);
}

TEST_F(SimExtractionDeviceTest, CoolsToAmbientAfterEndExtract) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(10.0, ExtractUnits::Watts));
  ASSERT_TRUE(laser.fire_laser());
  clock.advance(20s);
  ASSERT_TRUE(laser.end_extract());
  clock.advance(1s);
  EXPECT_NEAR(laser.temperature(), 20.0 + 300.0 * std::exp(-1.0), 1e-3);
}

TEST_F(SimExtractionDeviceTest, HotSampleReleasesGasThroughHook) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.move_to_position("2", false));
  clock.advance(1s);
  ASSERT_TRUE(laser.extract(100.0, ExtractUnits::Percent));  // 1520 C: far above c50
  ASSERT_TRUE(laser.fire_laser());
  clock.advance(30s);
  laser.update();
  ASSERT_FALSE(rec.releases.empty());
  EXPECT_EQ(rec.releases.back().position, "2");
  EXPECT_NEAR(rec.total(), 2.0, 1e-3);  // fully degassed
  EXPECT_NEAR(laser.released("2"), rec.total(), 1e-12);
}

TEST_F(SimExtractionDeviceTest, ReleaseDependsOnPeakTemperatureNotTime) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.move_to_position("1", false));
  clock.advance(1s);
  ASSERT_TRUE(laser.extract(700.0 / 30.0 - 20.0 / 30.0, ExtractUnits::Watts));  // 700 C = c50
  ASSERT_TRUE(laser.fire_laser());
  clock.advance(60s);
  laser.update();
  double at_c50 = rec.total();
  EXPECT_NEAR(at_c50, 1.0, 0.01);  // about half of 2.0
  clock.advance(600s);
  laser.update();
  EXPECT_NEAR(rec.total(), at_c50, 1e-3) << "holding temperature releases no more";
}

TEST_F(SimExtractionDeviceTest, NoSampleNoRelease) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(100.0, ExtractUnits::Percent));
  ASSERT_TRUE(laser.fire_laser());
  clock.advance(30s);
  laser.update();
  EXPECT_TRUE(rec.releases.empty());
}

TEST_F(SimExtractionDeviceTest, EachPositionHoldsItsOwnGas) {
  ASSERT_TRUE(laser.enable());
  for (auto p : {"1", "3"}) {
    ASSERT_TRUE(laser.move_to_position(p, false));
    clock.advance(1s);
    ASSERT_TRUE(laser.extract(100.0, ExtractUnits::Percent));
    ASSERT_TRUE(laser.fire_laser());
    clock.advance(30s);
    ASSERT_TRUE(laser.end_extract());
    clock.advance(30s);
  }
  laser.update();
  EXPECT_NEAR(laser.released("1"), 2.0, 1e-3);
  EXPECT_NEAR(laser.released("3"), 2.0, 1e-3);
  EXPECT_EQ(laser.released("2"), 0.0);
  EXPECT_NEAR(rec.total(), 4.0, 2e-3);
}

TEST_F(SimExtractionDeviceTest, StageMovesTakeTime) {
  ASSERT_TRUE(laser.move_to_position("3", false));
  EXPECT_EQ(extraction::conformance::val(laser.moving()), true);
  clock.advance(600ms);
  EXPECT_EQ(extraction::conformance::val(laser.moving()), false);
  auto unknown = laser.move_to_position("99", false);
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_FALSE(laser.set_tray("no-such-tray"));
  EXPECT_TRUE(laser.set_tray(laser_settings().trays.front()));
}

TEST_F(SimExtractionDeviceTest, PatternRunsForItsDuration) {
  ASSERT_TRUE(laser.execute_pattern("spiral"));
  EXPECT_EQ(extraction::conformance::val(laser.running()), true);
  clock.advance(3s);
  EXPECT_EQ(extraction::conformance::val(laser.running()), false);
}

TEST(SimFurnace, CelsiusSetpointsAndSampleDrop) {
  ManualClock clock;
  SimExtractionDevice furnace{furnace_settings(), clock};
  Recorder rec;
  furnace.on_gas_release([&](const GasRelease& r) { rec.releases.push_back(r); });
  EXPECT_EQ(extraction::capabilities(furnace), CapabilitySet{Capability::Furnace});
  ASSERT_TRUE(furnace.supports(ExtractUnits::Celsius));
  ASSERT_TRUE(furnace.enable());
  ASSERT_TRUE(furnace.drop_sample("1"));
  ASSERT_TRUE(furnace.extract(1200.0, ExtractUnits::Celsius));  // no firing needed
  clock.advance(30s);
  auto t = furnace.read_temperature();
  ASSERT_TRUE(t);
  EXPECT_NEAR(*t, 1200.0, 1e-3);
  EXPECT_NEAR(rec.total(), 2.0, 1e-3);
  ASSERT_TRUE(furnace.dump_sample());
  EXPECT_FALSE(furnace.drop_sample("nope"));
}

// --- conformance ------------------------------------------------------------

struct SimLaserHarness {
  ManualClock clock;
  SimExtractionDevice sim{laser_settings(), clock};
  extraction::IExtractionDevice& device() { return sim; }
  void advance() { clock.advance(100ms); }
};

struct SimFurnaceHarness {
  ManualClock clock;
  SimExtractionDevice sim{furnace_settings(), clock};
  extraction::IExtractionDevice& device() { return sim; }
  void advance() { clock.advance(100ms); }
};

}  // namespace
}  // namespace pychron::sim

using SimHarnesses = ::testing::Types<pychron::sim::SimLaserHarness, pychron::sim::SimFurnaceHarness>;
INSTANTIATE_TYPED_TEST_SUITE_P(Sim, ExtractionDeviceConformance, SimHarnesses);
INSTANTIATE_TYPED_TEST_SUITE_P(Sim, LaserConformance, ::testing::Types<pychron::sim::SimLaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Sim, StageConformance, ::testing::Types<pychron::sim::SimLaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Sim, PatternConformance, ::testing::Types<pychron::sim::SimLaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Sim, FurnaceConformance, ::testing::Types<pychron::sim::SimFurnaceHarness>);
