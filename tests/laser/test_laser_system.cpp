// LaserSystem over the Chromium driver and its simulator: hole names become
// stage millimetres through the tray map and its calibration (laser system
// design, section 3.4).

#include "pychron/laser/laser_system.hpp"

#include "laser_harness.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "extraction/conformance.hpp"
#include "extraction/fake.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/devices/extraction/chromium.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/transport/sim_transport.hpp"
#include "virtual_time.hpp"

using namespace pychron;
using namespace pychron::extraction;
using namespace pychron::laser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

using namespace pychron::laser::harness;

namespace {

struct FlippedXTest : LaserSystemTest {
  FlippedXTest() : LaserSystemTest(options({-1, 1, 1})) {}
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, ExtractionDeviceConformance, ::testing::Types<LaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, LaserConformance, ::testing::Types<LaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, StageConformance, ::testing::Types<LaserHarness>);
// conformance.hpp also defines a suite for a feature this device does not have.
INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, PatternConformance, ::testing::Types<LaserHarness>);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(FurnaceConformance);

TEST_F(LaserSystemTest, AHoleLandsOnItsCalibratedPosition) {
  ASSERT_TRUE(system.set_axis(IStage::Axis::Z, 1.5));
  settle();
  ASSERT_TRUE(system.move_to_position("3", false));  // (5, 0) on the tray
  EXPECT_TRUE(*system.moving());
  settle();
  const auto at = sim.position();
  EXPECT_EQ(at.x, 15000);
  EXPECT_EQ(at.y, 20000);
  EXPECT_EQ(at.z, 1500);  // z is not the tray's to change
  const auto read = system.position();
  ASSERT_TRUE(read);
  EXPECT_NEAR(read->x, 15, 1e-9);
  EXPECT_NEAR(read->y, 20, 1e-9);
}

TEST_F(LaserSystemTest, RotationIsApplied) {
  // The tray turned a quarter turn: map +x is stage +y.
  const std::vector<CalibrationPoint> points{{"1", 10, 20}, {"3", 10, 25}};
  ASSERT_TRUE(store.save(*trays.find("small"), "co2", points));
  ASSERT_TRUE(system.set_tray("small"));
  ASSERT_TRUE(system.move_to_position("2", false));  // (0, 5) on the tray
  settle();
  EXPECT_EQ(sim.position().x, 5000);
  EXPECT_EQ(sim.position().y, 20000);
}

TEST_F(FlippedXTest, SignsAreTheDrivers) {
  ASSERT_TRUE(system.move_to_position("3", false));
  EXPECT_TRUE(logged("Stage.MoveTo -15000,20000,0,5000,5000,100"));
}

TEST_F(LaserSystemTest, ScanNamesGoToTheDriver) {
  sim.add_scan({2000, 3000, 0});
  ASSERT_TRUE(system.move_to_position("s1", false));
  EXPECT_TRUE(logged("Scans.MoveTo 1"));
}

TEST_F(LaserSystemTest, AHoleNameWinsOverTheDriversNames) {
  // A tray with a hole called "s1" means that hole, not Chromium's scan 1.
  std::ofstream(lab.dir / "tray_maps" / "scans.txt") << "circle,1\n\n\ns1,0,0\ns2,1,0\n";
  const auto lib = TrayLibrary::load(lab.dir / "tray_maps");
  const std::vector<CalibrationPoint> points{{"s1", 3, 4}};
  ASSERT_TRUE(store.save(*lib.find("scans"), "co2", points));
  LaserSystem other{"co2", driver, lib, store};
  ASSERT_TRUE(other.set_tray("scans"));
  ASSERT_TRUE(other.move_to_position("s1", false));
  EXPECT_FALSE(logged("Scans.MoveTo 1"));
  EXPECT_TRUE(logged("Stage.MoveTo 3000,4000,0,5000,5000,100"));
}

TEST_F(LaserSystemTest, NoTrayIsRefusedAndSendsNothing) {
  ASSERT_TRUE(system.set_tray(""));
  const auto before = sim.log();
  auto r = system.move_to_position("3", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().device, "co2");
  EXPECT_NE(r.error().what.find("no tray"), std::string::npos) << r.error().what;
  EXPECT_EQ(sim.log(), before);
}

TEST_F(LaserSystemTest, AnUnknownHoleIsRefusedAndSendsNothing) {
  const auto before = sim.log();
  auto r = system.move_to_position("99", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().device, "co2");
  EXPECT_NE(r.error().what.find("99"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("small"), std::string::npos) << r.error().what;
  EXPECT_EQ(sim.log(), before);
}

TEST_F(LaserSystemTest, AnUncalibratedTrayIsRefusedAndSendsNothing) {
  ASSERT_TRUE(system.set_tray("bare"));  // a tray needs no calibration to be chosen
  EXPECT_EQ(system.calibration().state, CalibrationState::Missing);
  const auto before = sim.log();
  auto r = system.move_to_position("1", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_EQ(r.error().device, "co2");
  EXPECT_NE(r.error().what.find("bare"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("not calibrated"), std::string::npos) << r.error().what;
  EXPECT_EQ(sim.log(), before);
}

TEST_F(LaserSystemTest, AStaleCalibrationIsRefusedAndSendsNothing) {
  // The map is edited after the calibration was made.
  {
    std::ofstream out(lab.dir / "tray_maps" / "small.txt", std::ios::app);
    out << "20,20\n";
  }
  const auto edited = TrayLibrary::load(lab.dir / "tray_maps");
  LaserSystem other{"co2", driver, edited, store};
  ASSERT_TRUE(other.set_tray("small"));
  EXPECT_EQ(other.calibration().state, CalibrationState::Stale);
  const auto before = sim.log();
  auto r = other.move_to_position("3", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("stale"), std::string::npos) << r.error().what;
  EXPECT_EQ(sim.log(), before);
}

TEST_F(LaserSystemTest, UnknownTrayIsRefusedAndTheOldOneKept) {
  auto r = system.set_tray("no-such-tray");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("no-such-tray"), std::string::npos);
  EXPECT_EQ(system.tray(), "small");
  EXPECT_TRUE(system.move_to_position("3", false));
}

TEST_F(LaserSystemTest, AnEmptyTrayNameClearsIt) {
  ASSERT_TRUE(system.set_tray(""));
  EXPECT_EQ(system.tray(), "");
  EXPECT_TRUE(system.positions().empty());
  EXPECT_EQ(system.calibration().state, CalibrationState::Missing);
}

TEST_F(LaserSystemTest, PositionsAreTheTraysHoles) {
  EXPECT_EQ(system.positions(), (std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7", "8", "A"}));
}

TEST_F(LaserSystemTest, ACalibrationSavedLaterIsSeenAtTheNextSetTray) {
  ASSERT_TRUE(system.set_tray("bare"));
  ASSERT_FALSE(system.move_to_position("2", false));
  // another process calibrates (elctl laser calibrate) while this one runs
  const std::vector<CalibrationPoint> points{{"1", 30, 30}};
  ASSERT_TRUE(store.save(*trays.find("bare"), "co2", points));
  ASSERT_FALSE(system.move_to_position("2", false));  // not until the tray is set again
  ASSERT_TRUE(system.set_tray("bare"));
  ASSERT_TRUE(system.move_to_position("2", false));
  settle();
  EXPECT_EQ(sim.position().x, 31000);
  EXPECT_EQ(sim.position().y, 30000);
}

TEST_F(LaserSystemTest, AHoleOutsideTravelIsTheDriversRefusal) {
  calibrate(49, 0);  // hole 3 is then at x = 54, past the 50 mm limit
  ASSERT_TRUE(system.set_tray("small"));
  const auto before = sim.position();
  const auto log = sim.log();
  auto r = system.move_to_position("3", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("hole 3"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("small"), std::string::npos) << r.error().what;
  clock.advance(5s);
  EXPECT_EQ(sim.position().x, before.x);
  EXPECT_EQ(sim.position().y, before.y);
  for (std::size_t i = log.size(); i < sim.log().size(); ++i) {
    EXPECT_FALSE(sim.log()[i].starts_with("Stage.MoveTo")) << sim.log()[i];
  }
  EXPECT_FALSE(*system.moving());
}

TEST_F(LaserSystemTest, SpeedAndStopAreTheDrivers) {
  ASSERT_TRUE(system.set_xy(10, 0, 1.0));
  EXPECT_TRUE(logged("Stage.MoveTo 10000,0,0,1000,1000,100"));
  clock.advance(2s);
  ASSERT_TRUE(system.stop());
  EXPECT_TRUE(logged("Stage.Stop"));
  EXPECT_FALSE(*system.moving());
  // a hole move is at the stage's own speed
  ASSERT_TRUE(system.move_to_position("3", false));
  EXPECT_TRUE(logged("Stage.MoveTo 15000,20000,0,5000,5000,100"));
}

// What the system's simulated camera is told: where the stage is, where the
// holes are, and whether the beam is on.
TEST_F(LaserSystemTest, SightSaysWhereTheStageIsAndWhetherTheLaserFires) {
  const auto sight = system.sight();
  ASSERT_TRUE(system.set_xy(3, 4));
  settle();
  auto seen = sight();
  EXPECT_NEAR(seen.stage.x, 3, 1e-9);
  EXPECT_NEAR(seen.stage.y, 4, 1e-9);
  EXPECT_EQ(seen.holes.size(), 9u);
  EXPECT_NEAR(seen.holes[2].x, 15, 1e-9);  // hole 3
  EXPECT_FALSE(seen.firing);
  ASSERT_TRUE(system.enable());
  ASSERT_TRUE(system.extract(20, ExtractUnits::Percent));
  ASSERT_TRUE(system.laser()->fire_laser());
  seen = sight();
  EXPECT_TRUE(seen.firing);
  EXPECT_DOUBLE_EQ(seen.output_percent, 20);
  ASSERT_TRUE(system.end_extract());
  EXPECT_FALSE(sight().firing);
}

TEST_F(LaserSystemTest, AutocenterIsAcceptedAndChangesNothing) {
  ASSERT_TRUE(system.move_to_position("3", true));
  EXPECT_TRUE(logged("Stage.MoveTo 15000,20000,0,5000,5000,100"));
}

TEST_F(LaserSystemTest, TheDeviceIsTheDrivers) {
  EXPECT_EQ(system.device_name(), "co2");
  // the laser is reached through the system too: it is what gates and stops it
  EXPECT_EQ(system.laser(), static_cast<ILaserDevice*>(&system));
  EXPECT_EQ(system.stage(), static_cast<IStage*>(&system));
  EXPECT_EQ(system.furnace(), nullptr);
  ASSERT_TRUE(system.enable());
  EXPECT_TRUE(*driver.is_enabled());
  ASSERT_TRUE(system.extract(20, ExtractUnits::Percent));
  EXPECT_DOUBLE_EQ(*driver.output(), 20);
  EXPECT_DOUBLE_EQ(*system.output(), 20);
  EXPECT_TRUE(system.supports(ExtractUnits::Percent));
  EXPECT_FALSE(system.supports(ExtractUnits::Watts));
  ASSERT_TRUE(system.disable());
  EXPECT_FALSE(*driver.is_enabled());
}

TEST(LaserSystemFeatures, ADriverWithoutAStageHasNone) {
  LabDir lab;
  const auto trays = TrayLibrary::load(lab.dir / "tray_maps");
  const CalibrationStore store{lab.dir / "stage_calibrations"};
  extraction::testing::FakeExtractionDevice furnace{"furnace", {Capability::Furnace}};
  LaserSystem system{"furnace", furnace, trays, store};
  EXPECT_EQ(system.stage(), nullptr);
  EXPECT_EQ(system.furnace(), furnace.furnace());
  EXPECT_EQ(system.laser(), nullptr);
  // called anyway (through a kept pointer), nothing crashes
  auto r = static_cast<IStage&>(system).move_to_position("1", false);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_FALSE(static_cast<IStage&>(system).position());
  EXPECT_FALSE(static_cast<IStage&>(system).stop());
}

// A stage that cannot stop says so, through the system as on its own.
TEST(LaserSystemFeatures, AStageThatCannotStopSaysSo) {
  LabDir lab;
  const auto trays = TrayLibrary::load(lab.dir / "tray_maps");
  const CalibrationStore store{lab.dir / "stage_calibrations"};
  extraction::testing::FakeExtractionDevice fake{"fake", {Capability::Stage}};
  LaserSystem system{"fake", fake, trays, store};
  ASSERT_NE(system.stage(), nullptr);
  auto r = system.stage()->stop();
  ASSERT_FALSE(r);
  EXPECT_TRUE(is_not_supported(r.error()));
}

// ---- On a VirtualClock: the test's thread takes part in the clock's time ----

namespace {

struct LaserSystemVirtual : pychron::testing::VirtualTimeTest {};

}  // namespace

// A call holds the gate while the device answers, and a slow answer is clock
// time. A second caller (the window's watcher beside a script) waits for the
// gate through the clock: blocked any other way it looks runnable, time
// stands, and the answer never comes.
TEST_F(LaserSystemVirtual, TwoCallersContendWithoutStallingTime) {
  LabDir lab;
  VirtualClock clock;
  Clock::Participant main(clock, "test");
  ChromiumSim sim{clock};
  // The simulator answers at once; this wire makes two of its answers slow.
  std::atomic<int> slow_answers{0};
  const auto slowly = [&](const Bytes& tx) {
    const std::string text = to_string(tx);
    const Duration takes = text.starts_with("Laser.Enable?") ? Duration(2s)
                           : text.starts_with("Stage.Pos?")  ? Duration(1s)
                                                             : Duration(0s);
    if (takes > 0s) {
      ++slow_answers;
      clock.sleep_for(takes);
    }
    return sim.hook()(tx);
  };
  auto wire = SimTransport::hooked(slowly, TransportOptions{.name = "laser_pc", .clock = &clock});
  ASSERT_TRUE(wire->open());
  ChromiumLaser driver("co2", *wire, options());
  const TrayLibrary trays = TrayLibrary::load(lab.dir / "tray_maps");
  const CalibrationStore store{lab.dir / "stage_calibrations"};
  LaserSystem system{"co2", driver, trays, store, nullptr, &clock};
  const TimePoint start = clock.now();
  const auto real_start = std::chrono::steady_clock::now();

  TimePoint asked{}, read{};
  bool enabled_ok = false, position_ok = false;
  pychron::testing::Crew crew(clock);
  crew.start("script", [&] {
    enabled_ok = system.is_enabled().has_value();
    asked = clock.now();
  });
  // The first caller has the gate, and its question is with the device.
  ASSERT_TRUE(pychron::testing::eventually_real([&] { return slow_answers.load() == 1; }));
  crew.start("watcher", [&] {
    position_ok = system.position().has_value();
    read = clock.now();
  });
  crew.join();

  EXPECT_TRUE(enabled_ok);
  EXPECT_TRUE(position_ok);
  // One after the other: two seconds for the first answer, then one for the
  // second, which was not asked for until the first caller let go.
  EXPECT_EQ(asked, start + 2s);
  EXPECT_EQ(read, start + 3s);
  EXPECT_EQ(slow_answers.load(), 2);
  EXPECT_EQ(clock.now(), start + 3s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_start, 5s);
}

// A system given a clock waits for its gate on it, and its camera's settle
// and stamps are read from the camera's: two clocks in one system would let
// simulated time and real time meet. Refused where asserts are on.
TEST(LaserSystemClocks, ACameraIsOnTheClockTheSystemWasGiven) {
  LabDir lab;
  ManualClock clock;
  ManualClock other;
  ChromiumSim sim{clock};
  auto wire = SimTransport::hooked(sim.hook(), TransportOptions{.name = "laser_pc", .clock = &clock});
  ASSERT_TRUE(wire->open());
  ChromiumLaser driver("co2", *wire, options());
  const TrayLibrary trays = TrayLibrary::load(lab.dir / "tray_maps");
  const CalibrationStore store{lab.dir / "stage_calibrations"};
  LaserSystem system{"co2", driver, trays, store, nullptr, &clock};
  const CameraConfig config = camera_config();
  EXPECT_TRUE(system.attach_viewer(config, std::make_unique<SimTrayCamera>(config, system.sight(), clock), clock));
#ifndef NDEBUG
  GTEST_FLAG_SET(death_test_style, "threadsafe");  // the process has other threads
  EXPECT_DEATH(
      (void)system.attach_viewer(config, std::make_unique<SimTrayCamera>(config, system.sight(), other), other),
      "gate_clock_");
  EXPECT_DEATH(
      (void)system.attach_camera(config, std::make_unique<SimTrayCamera>(config, system.sight(), other), other),
      "gate_clock_");
#endif
}
