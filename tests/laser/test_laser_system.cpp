// LaserSystem over the Chromium driver and its simulator: hole names become
// stage millimetres through the tray map and its calibration (laser system
// design, section 3.4).

#include "pychron/laser/laser_system.hpp"

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
#include "pychron/devices/extraction/chromium.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace pychron::extraction;
using namespace pychron::laser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

ChromiumOptions options(std::array<int, 3> signs = {1, 1, 1}) {
  ChromiumOptions o;
  o.limits_mm = {{{-50, 50}, {-50, 50}, {-50, 50}}};
  o.signs = signs;
  return o;
}

// A lab directory on disk with the small tray (holes 1 (0,0), 2 (0,5),
// 3 (5,0), ...), calibrated for "co2" with the tray's origin at stage
// (10, 20) and no rotation.
struct LabDir {
  fs::path dir;
  LabDir() {
    std::random_device rd;
    dir = fs::temp_directory_path() / ("pychron_laser_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir / "tray_maps");
    fs::copy_file(fs::path(PYCHRON_TEST_DATA_DIR) / "tray_maps" / "small.txt", dir / "tray_maps" / "small.txt");
    std::ofstream(dir / "tray_maps" / "bare.txt") << "circle,1\n\n\n1,0,0\n2,1,0\n";  // never calibrated
  }
  ~LabDir() { fs::remove_all(dir); }
};

// Declared in the order they must be built and, reversed, torn down.
struct LaserHarness {
  LabDir lab;
  ManualClock clock;
  ChromiumSim sim{clock};
  std::unique_ptr<SimTransport> wire = SimTransport::hooked(sim.hook(), TransportOptions{.name = "laser_pc", .clock = &clock});
  ChromiumLaser driver;
  TrayLibrary trays = TrayLibrary::load(lab.dir / "tray_maps");
  CalibrationStore store{lab.dir / "stage_calibrations"};
  LaserSystem system{"co2", driver, trays, store};

  explicit LaserHarness(ChromiumOptions o = options()) : driver("co2", *wire, o) {
    EXPECT_TRUE(wire->open());
    EXPECT_TRUE(trays.problems().empty());
    calibrate(10, 20);
    EXPECT_TRUE(system.set_tray("small"));
  }
  void calibrate(double cx, double cy) {
    const std::vector<CalibrationPoint> points{{"1", cx, cy}, {"3", cx + 5, cy}};
    auto saved = store.save(*trays.find("small"), "co2", points);
    EXPECT_TRUE(saved) << (saved ? "" : saved.error().what);
  }
  IExtractionDevice& device() { return system; }
  void advance() { clock.advance(250ms); }
};

struct LaserSystemTest : ::testing::Test, LaserHarness {
  using LaserHarness::LaserHarness;
  bool logged(std::string_view command) const {
    const auto log = sim.log();
    return std::find(log.begin(), log.end(), command) != log.end();
  }
  void settle() { ASSERT_TRUE(conformance::settles(*this, [&] { return system.moving(); })); }
};

struct FlippedXTest : LaserSystemTest {
  FlippedXTest() : LaserSystemTest(options({-1, 1, 1})) {}
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, ExtractionDeviceConformance, ::testing::Types<LaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, LaserConformance, ::testing::Types<LaserHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(LaserSystem, StageConformance, ::testing::Types<LaserHarness>);
// conformance.hpp also defines suites for features this device does not have.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(PatternConformance);
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

TEST_F(LaserSystemTest, AutocenterIsAcceptedAndChangesNothing) {
  ASSERT_TRUE(system.move_to_position("3", true));
  EXPECT_TRUE(logged("Stage.MoveTo 15000,20000,0,5000,5000,100"));
}

TEST_F(LaserSystemTest, TheDeviceIsTheDrivers) {
  EXPECT_EQ(system.device_name(), "co2");
  EXPECT_EQ(system.laser(), driver.laser());
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
