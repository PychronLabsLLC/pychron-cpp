#pragma once

// The laser system on the Chromium driver and its simulator, in a scratch
// lab directory: shared by the LaserSystem and PatternRunner tests.

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "extraction/conformance.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/devices/extraction/chromium.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/laser/laser_system.hpp"
#include "pychron/laser/pattern.hpp"
#include "pychron/transport/sim_transport.hpp"

namespace pychron::laser::harness {

using namespace pychron::extraction;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

inline ChromiumOptions options(std::array<int, 3> signs = {1, 1, 1}) {
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
    fs::create_directories(dir / "patterns");
    const auto pattern = [this](const char* name, const char* text) { std::ofstream(dir / "patterns" / name) << text; };
    pattern("square.toml", "kind = \"polygon\"\nradius = 1\nnsides = 4\nvelocity = 2\n");
    pattern("twice.toml", "kind = \"polygon\"\nradius = 1\nnsides = 4\nvelocity = 2\niterations = 2\n");
    pattern("wide.toml", "kind = \"polygon\"\nradius = 60\nnsides = 4\n");  // outside the stage's +-50 mm
    pattern("walk.toml", "kind = \"random\"\nnpoints = 4\nseed = 7\nvelocity = 5\n");
    pattern("broken.toml", "kind = \"polygon\"\nradius = 0\n");
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
  PatternLibrary patterns = PatternLibrary::load(lab.dir / "patterns");
  LaserSystem system{"co2", driver, trays, store, &patterns};

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

}  // namespace pychron::laser::harness
