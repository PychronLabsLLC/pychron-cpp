// elctl laser: trays, calibrate, goto, on a scratch copy of the example lab
// (its "co2" is a Chromium driver on a simulated transport).

#include "pychron/vision/camera_backend.hpp"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "elctl_fixture.hpp"
#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/tray_map.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/synth.hpp"

namespace elctl::testing {
namespace {

namespace fs = std::filesystem;
using pychron::laser::CalibrationPoint;

class LaserCmd : public ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_ / "lab",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    fs::remove_all(dir_ / "lab" / "data");
  }
  fs::path lab(const std::string& f) const { return dir_ / "lab" / f; }
  fs::path calibration() const { return lab("stage_calibrations") / "co2.example-9.toml"; }

  Outcome laser(std::vector<std::string> args, bool sim = false) const {
    std::vector<std::string> all{"-c", lab("extraction_line.toml").string()};
    if (sim) all.push_back("--sim");
    all.push_back("laser");
    all.insert(all.end(), args.begin(), args.end());
    return run_raw(all);
  }
  Outcome calibrate(std::vector<std::string> args, bool sim = false) const {
    args.insert(args.begin(), {"calibrate", "co2", "example-9"});
    return laser(std::move(args), sim);
  }
  std::vector<CalibrationPoint> points() const {
    const pychron::laser::CalibrationStore store(lab("stage_calibrations"));
    const auto loaded = store.load("co2", "example-9");
    if (!loaded || !loaded->has_value()) return {};
    return (*loaded)->points;
  }
  static std::string slurp(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }
  // The laser PC is somewhere that will never answer: any attempt to open it fails.
  void unplug_the_laser() const {
    std::string text = slurp(lab("extraction_line.toml"));
    const std::string sim = "[transports.laser_pc]\nkind = \"sim\"";
    const auto at = text.find(sim);
    ASSERT_NE(at, std::string::npos);
    text.replace(at, sim.size(), "[transports.laser_pc]\nkind = \"tcp\"\nhost = \"127.0.0.1\"\nport = 1");
    std::ofstream(lab("extraction_line.toml"), std::ios::trunc) << text;
  }
};

TEST_F(LaserCmd, TraysListsMapsAndCalibrationState) {
  std::ofstream(lab("tray_maps") / "spare.txt") << "circle,1\n\n\n0,0\n1,0\n";
  const auto o = laser({"trays"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("example-9"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("9 holes"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("co2: calibrated (2 points, rms 0.000 mm)"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("spare"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("2 holes"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("co2: not calibrated"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, TraysReportsAStaleCalibrationAndABadMap) {
  std::ofstream(lab("tray_maps") / "example-9.txt", std::ios::app) << "10,10\n";
  std::ofstream(lab("tray_maps") / "broken.txt") << "circle,1\n\n\n1,x\n";
  const auto o = laser({"trays"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.out.find("co2: stale"), std::string::npos) << o.out;
  EXPECT_NE(o.err.find("broken:4"), std::string::npos) << o.err;
}

TEST_F(LaserCmd, PointWithNumbersOpensNoHardware) {
  unplug_the_laser();
  const auto o = calibrate({"point", "3", "--x", "30.5", "--y", "29.75"});
  ASSERT_EQ(o.code, 0) << o.err;
  const auto p = points();
  ASSERT_EQ(p.size(), 3u);
  EXPECT_EQ(p[2], (CalibrationPoint{"3", 30.5, 29.75}));
  EXPECT_NE(o.out.find("3 points"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("rms"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, PointReadsTheLiveStage) {
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  const auto o = calibrate({"point", "5"}, true);  // the simulated stage rests at 0, 0
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_EQ(points(), (std::vector<CalibrationPoint>{{"5", 0, 0}}));
  EXPECT_NE(o.out.find("stage at 0.000, 0.000"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, ReadingTheStageOfAnUnpluggedLaserFails) {
  unplug_the_laser();
  const std::string before = slurp(calibration());
  const auto o = calibrate({"point", "5"});
  EXPECT_EQ(o.code, 1);
  EXPECT_FALSE(o.err.empty());
  EXPECT_EQ(slurp(calibration()), before);
}

TEST_F(LaserCmd, PointReplacesAnEarlierPointOnTheSameHole) {
  ASSERT_EQ(calibrate({"point", "5", "--x", "25.5", "--y", "25"}).code, 0);
  EXPECT_EQ(points(), (std::vector<CalibrationPoint>{{"5", 25.5, 25}, {"6", 30, 25}}));
}

TEST_F(LaserCmd, CenterAndRightUseTheMapsHoles) {
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  ASSERT_EQ(calibrate({"center", "--x", "10", "--y", "10"}).code, 0);
  const auto o = calibrate({"right", "--x", "10", "--y", "15"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_EQ(points(), (std::vector<CalibrationPoint>{{"5", 10, 10}, {"6", 10, 15}}));
  EXPECT_NE(o.out.find("rotation 90.000 deg"), std::string::npos) << o.out;
}

// Centre and right exchanged fit perfectly, half a turn round: it must be said.
TEST_F(LaserCmd, ExchangedPointsAreWarnedAbout) {
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  ASSERT_EQ(calibrate({"center", "--x", "30", "--y", "25"}).code, 0);
  const auto o = calibrate({"right", "--x", "25", "--y", "25"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("rms 0.000 mm"), std::string::npos) << o.out;
  EXPECT_NE(o.err.find("warning"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("180"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("mirror"), std::string::npos) << o.err;
  // and it is still said later
  EXPECT_NE(calibrate({"show"}).err.find("180"), std::string::npos);
  EXPECT_NE(laser({"trays"}).out.find("check"), std::string::npos);
}

TEST_F(LaserCmd, CenterWithoutCalibrationHolesIsAnError) {
  std::ofstream(lab("tray_maps") / "plain.txt") << "circle,1\n\n\n0,0\n1,0\n";
  const auto o = laser({"calibrate", "co2", "plain", "center", "--x", "1", "--y", "1"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("plain"), std::string::npos) << o.err;
  EXPECT_FALSE(fs::exists(lab("stage_calibrations") / "co2.plain.toml"));
}

TEST_F(LaserCmd, APointThatDoesNotSolveLeavesTheFile) {
  const std::string before = slurp(calibration());
  const auto o = calibrate({"point", "1", "--x", "25", "--y", "25"});  // where hole 5 already is
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("same stage position"), std::string::npos) << o.err;
  EXPECT_EQ(slurp(calibration()), before);
}

TEST_F(LaserCmd, APoorFitIsSavedWithAWarning) {
  // hole 6 is 5 mm from hole 5 on the map; here the stage says 9 mm.
  const auto o = calibrate({"point", "6", "--x", "34", "--y", "25"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.err.find("warning"), std::string::npos) << o.err;
  EXPECT_NE(o.out.find("rms 4.000 mm"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, UsageErrors) {
  EXPECT_EQ(laser({}).code, 2);
  EXPECT_EQ(laser({"nope"}).code, 2);
  EXPECT_EQ(calibrate({}).code, 2);
  EXPECT_EQ(calibrate({"point"}).code, 2);
  EXPECT_EQ(calibrate({"point", "5", "--x", "1"}).code, 2);  // one of x and y
  EXPECT_EQ(calibrate({"point", "5", "--x", "one", "--y", "2"}).code, 2);
  EXPECT_EQ(calibrate({"center", "5"}).code, 2);
  EXPECT_EQ(calibrate({"show", "extra"}).code, 2);
  EXPECT_EQ(laser({"goto", "co2", "example-9"}).code, 2);
  EXPECT_EQ(points().size(), 2u);
}

TEST_F(LaserCmd, ShowPrintsPointsAndSolution) {
  const auto o = calibrate({"show"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("hole 5"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("25.000, 25.000"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("hole 6"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("centre 25.000, 25.000"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("rotation 0.000 deg"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("scale 1.0000"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, ClearRemovesTheFile) {
  ASSERT_TRUE(fs::exists(calibration()));
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  EXPECT_FALSE(fs::exists(calibration()));
  const auto o = calibrate({"show"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.out.find("not calibrated"), std::string::npos) << o.out << o.err;
}

TEST_F(LaserCmd, PointsAgainstAnEditedMapStartAgain) {
  std::ofstream(lab("tray_maps") / "example-9.txt", std::ios::app) << "10,10\n";
  const auto o = calibrate({"point", "5", "--x", "1", "--y", "2"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.err.find("changed"), std::string::npos) << o.err;
  EXPECT_EQ(points(), (std::vector<CalibrationPoint>{{"5", 1, 2}}));
}

TEST_F(LaserCmd, GotoMovesAndReportsTheMiss) {
  // The tray near the stage's rest position, so the simulated move is short.
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  ASSERT_EQ(calibrate({"center", "--x", "1", "--y", "1"}).code, 0);
  ASSERT_EQ(calibrate({"right", "--x", "6", "--y", "1"}).code, 0);
  const auto o = laser({"goto", "co2", "example-9", "5"}, true);
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("hole 5"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("1.000, 1.000"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("miss 0.000 mm"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, GotoUncalibratedIsRefused) {
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  const auto o = laser({"goto", "co2", "example-9", "5"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("not calibrated"), std::string::npos) << o.err;
}

TEST_F(LaserCmd, GotoGivesUpAfterItsTimeout) {
  const auto o = laser({"goto", "co2", "example-9", "5", "--timeout", "0.3"}, true);  // 25 mm away at 5 mm/s
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("still moving after 0.3 s"), std::string::npos) << o.err;
  // and it does not leave the stage going: it is stopped part way
  EXPECT_NE(o.err.find("the stage was stopped at "), std::string::npos) << o.err;
  EXPECT_EQ(o.err.find("may still be moving"), std::string::npos) << o.err;
  EXPECT_EQ(o.err.find("stopped at 25.000, 25.000"), std::string::npos) << o.err;
}

TEST_F(LaserCmd, UnknownNamesSayWhatIsKnown) {
  auto o = laser({"calibrate", "diode", "example-9", "show"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("diode"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("co2"), std::string::npos) << o.err;

  o = laser({"calibrate", "co2", "221-hole", "show"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("221-hole"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("example-9"), std::string::npos) << o.err;

  o = calibrate({"point", "99", "--x", "1", "--y", "1"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("99"), std::string::npos) << o.err;
}

// --- patterns ---------------------------------------------------------------

TEST_F(LaserCmd, PatternsListsKindPointsLengthAndTime) {
  const auto o = laser({"patterns"});
  ASSERT_EQ(o.code, 0) << o.err;
  // radius 1: out to a vertex, six sides of 1 mm, and back; 1 mm/s
  EXPECT_NE(o.out.find("hexagon  polygon  8 points  8.000 mm  8.0 s"), std::string::npos) << o.out;
}

// A dragonfly has no path to list or to walk: it follows the glow.
TEST_F(LaserCmd, PatternsListsADragonfly) {
  const auto o = laser({"patterns"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("follow  dragonfly  follows the glow for the run's duration (else 5.0 s) within 2.500 mm"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("hexagon  polygon"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, PatternDryRunOfADragonflySaysItHasNoPath) {
  unplug_the_laser();
  const auto o = laser({"pattern", "co2", "follow", "--dry-run"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("no path"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("5.0 s"), std::string::npos) << o.out;
}

// It steers by the glow of a heated sample, and this command does not fire
// the laser: there would be nothing to follow.
TEST_F(LaserCmd, ADragonflyIsNotRunFromHere) {
  const auto o = laser({"pattern", "co2", "follow"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("glow"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("queue"), std::string::npos) << o.err;
}

TEST_F(LaserCmd, PatternsReportsAFileThatDidNotLoad) {
  std::ofstream(lab("patterns") / "broken.toml") << "kind = \"polygon\"\nradius = 0\n";
  const auto o = laser({"patterns"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.out.find("hexagon"), std::string::npos) << o.out;
  EXPECT_NE(o.err.find("broken: radius"), std::string::npos) << o.err;
}

// Listing patterns is about a folder of files: it needs no line config.
TEST_F(LaserCmd, PatternsNeedsNoLineConfig) {
  const auto o = run_raw({"laser", "patterns", "--lab", (dir_ / "lab").string()});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("hexagon  polygon"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, PatternsWithNoneSaysSo) {
  fs::remove_all(lab("patterns"));
  const auto o = laser({"patterns"});
  EXPECT_EQ(o.code, 0);
  EXPECT_NE(o.out.find("no patterns"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, PatternDryRunPrintsThePointsAndOpensNoHardware) {
  unplug_the_laser();
  const auto o = laser({"pattern", "co2", "hexagon", "--dry-run"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("  1    1.000,  0.000"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("  2    0.500,  0.866"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("  4   -1.000,  0.000"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("  8    0.000,  0.000"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("8 points  8.000 mm  8.0 s at 1.000 mm/s"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, PatternRunsOnTheSimulatorAndReturns) {
  // The simulated stage rests at the corner of its travel: a pattern that
  // stays inside it.
  std::ofstream(lab("patterns") / "line.toml") << "kind = \"linear\"\nlength = 1\nvelocity = 5\n";
  const auto o = laser({"pattern", "co2", "line"}, true);
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("pattern line: 3 points"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("ended at 0.000, 0.000"), std::string::npos) << o.out;
}

TEST_F(LaserCmd, APatternOffTheStageFailsAndSaysWhichPoint) {
  const auto o = laser({"pattern", "co2", "hexagon"}, true);  // about (0, 0): half of it is outside
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("pattern hexagon, point "), std::string::npos) << o.err;
}

TEST_F(LaserCmd, PatternTimeoutStopsTheStage) {
  std::ofstream(lab("patterns") / "crawl.toml") << "kind = \"linear\"\nlength = 5\nvelocity = 0.5\n";
  const auto o = laser({"pattern", "co2", "crawl", "--timeout", "0.3"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("still running after 0.3 s"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("the stage was stopped at "), std::string::npos) << o.err;
}

TEST_F(LaserCmd, UnknownPatternNamesTheKnownOnes) {
  auto o = laser({"pattern", "co2", "spiral", "--dry-run"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("spiral"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("hexagon"), std::string::npos) << o.err;
  o = laser({"pattern", "diode", "hexagon", "--dry-run"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("diode"), std::string::npos) << o.err;
}

TEST_F(LaserCmd, PatternUsageErrors) {
  EXPECT_EQ(laser({"pattern"}).code, 2);
  EXPECT_EQ(laser({"pattern", "co2"}).code, 2);
  EXPECT_EQ(laser({"pattern", "co2", "hexagon", "extra"}).code, 2);
  EXPECT_EQ(laser({"patterns", "extra"}).code, 2);
  EXPECT_EQ(laser({"patterns", "--dry-run"}).code, 2);
  EXPECT_EQ(laser({"trays", "--dry-run"}).code, 2);
  EXPECT_EQ(laser({"pattern", "co2", "hexagon", "--x", "1", "--y", "1"}).code, 2);
}

// --- autocenter, corrections, look ---------------------------------------------

// The example's camera sees the tray 0.15, -0.10 mm from where it is
// calibrated. The tray is calibrated near the stage's rest position so the
// simulated moves are short: hole 5 at (1, 1), really at (1.15, 0.90).
struct LaserCameraCmd : LaserCmd {
  void SetUp() override {
    LaserCmd::SetUp();
    ASSERT_EQ(calibrate({"clear"}).code, 0);
    ASSERT_EQ(calibrate({"center", "--x", "1", "--y", "1"}).code, 0);
    ASSERT_EQ(calibrate({"right", "--x", "6", "--y", "1"}).code, 0);
  }
  void camera(const std::string& text) const { std::ofstream(lab("cameras.toml"), std::ios::trunc) << text; }
  fs::path corrections_file() const { return lab("stage_corrections") / "co2.example-9.toml"; }
};

TEST_F(LaserCameraCmd, AutocenterCentresAndSaves) {
  const auto o = laser({"autocenter", "co2", "example-9", "5"}, true);
  ASSERT_EQ(o.code, 0) << o.err << o.out;
  EXPECT_NE(o.out.find("hole 5: converged"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("moved 0.1"), std::string::npos) << o.out;   // 0.15 in x, give or take the tolerance
  EXPECT_NE(o.out.find(", -0."), std::string::npos) << o.out;       // and down in y
  EXPECT_NE(o.out.find("residual 0.0"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("now at 1.1"), std::string::npos) << o.out;
  ASSERT_TRUE(fs::exists(corrections_file()));

  const auto listed = laser({"corrections", "co2", "example-9"});
  ASSERT_EQ(listed.code, 0) << listed.err;
  EXPECT_NE(listed.out.find("hole 5  1.1"), std::string::npos) << listed.out;
  EXPECT_NE(listed.out.find("mm from calibrated"), std::string::npos) << listed.out;
  EXPECT_NE(listed.out.find("0.18"), std::string::npos) << listed.out;  // hypot(0.15, 0.10)
}

TEST_F(LaserCameraCmd, AutocenterFailureIsExitOneWhateverTheConfigSays) {
  camera("[co2]\n[co2.sim]\ntray_error_mm = [3.0, 3.0]\nnoise = 0\n[co2.autocenter]\non_failure = \"continue\"\n");
  const auto o = laser({"autocenter", "co2", "example-9", "5"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("no_target"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("hole 5"), std::string::npos) << o.err;
  EXPECT_FALSE(fs::exists(corrections_file()));
}

TEST_F(LaserCameraCmd, AutocenterNeedsACamera) {
  fs::remove(lab("cameras.toml"));
  auto o = laser({"autocenter", "co2", "example-9", "5"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("cameras.toml"), std::string::npos) << o.err;
  EXPECT_NE(o.err.find("co2"), std::string::npos) << o.err;
  // a table that does not load says what is wrong with it
  camera("[co2]\npx_per_mm = 0\n");
  o = laser({"autocenter", "co2", "example-9", "5"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("co2.px_per_mm"), std::string::npos) << o.err;
}

// A recording does not follow the stage: it is for `look`, never for moving.
TEST_F(LaserCameraCmd, AutocenterRefusesARecordedCamera) {
  const auto dir = lab("recordings") / "holes";
  fs::create_directories(dir);
  {
    pychron::vision::FrameRecorder recorder(dir, pychron::vision::Provenance::Synthetic, pychron::vision::FinderMode::Hole, 23);
    pychron::vision::HoleScene scene;
    scene.hole_radius_mm = 1.0;
    scene.hole_mm = {0.5, 0};
    for (int i = 0; i < 12; ++i) ASSERT_TRUE(recorder.add(pychron::vision::render(scene, {0, 0}).first.view()));
    ASSERT_TRUE(recorder.finish());
  }
  camera("[co2]\nsource = \"recorded\"\nframes = \"recordings/holes\"\n");
  const auto o = laser({"autocenter", "co2", "example-9", "5"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("recorded"), std::string::npos) << o.err;
  EXPECT_EQ(o.out.find("converged"), std::string::npos) << o.out;
  EXPECT_FALSE(fs::exists(corrections_file()));
}

TEST_F(LaserCameraCmd, AutocenterGivesUpAfterItsTimeoutAndStopsTheStage) {
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  ASSERT_EQ(calibrate({"center", "--x", "25", "--y", "25"}).code, 0);  // 5 s away
  const auto o = laser({"autocenter", "co2", "example-9", "5", "--timeout", "0.3"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("the stage was stopped"), std::string::npos) << o.err;
  EXPECT_FALSE(fs::exists(corrections_file()));
}

TEST_F(LaserCameraCmd, CorrectionsClearOneAndAll) {
  ASSERT_EQ(laser({"autocenter", "co2", "example-9", "5"}, true).code, 0);
  ASSERT_EQ(laser({"autocenter", "co2", "example-9", "6"}, true).code, 0);
  auto listed = laser({"corrections", "co2", "example-9"});
  EXPECT_NE(listed.out.find("hole 5"), std::string::npos);
  EXPECT_NE(listed.out.find("hole 6"), std::string::npos);

  ASSERT_EQ(laser({"corrections", "co2", "example-9", "clear", "5"}).code, 0);
  listed = laser({"corrections", "co2", "example-9"});
  EXPECT_EQ(listed.out.find("hole 5"), std::string::npos) << listed.out;
  EXPECT_NE(listed.out.find("hole 6"), std::string::npos);

  ASSERT_EQ(laser({"corrections", "co2", "example-9", "clear"}).code, 0);
  EXPECT_FALSE(fs::exists(corrections_file()));
  listed = laser({"corrections", "co2", "example-9"});
  EXPECT_EQ(listed.code, 0);
  EXPECT_NE(listed.out.find("no corrections"), std::string::npos) << listed.out;
}

TEST_F(LaserCameraCmd, CorrectionsOfAnUncalibratedTraySaysSo) {
  ASSERT_EQ(calibrate({"clear"}).code, 0);
  const auto o = laser({"corrections", "co2", "example-9"});
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("not calibrated"), std::string::npos) << o.err;
}

// What the finder sees, with nothing moved: at rest (0, 0) the stage is 1.4 mm
// from hole 5's real place.
TEST_F(LaserCameraCmd, LookSaysWhatTheFinderSees) {
  const auto o = laser({"look", "co2", "--tray", "example-9"}, true);
  ASSERT_EQ(o.code, 0) << o.err;
  // hole 5 really at (1.15, 0.90): right of centre, and above (image y is down)
  EXPECT_NE(o.out.find("offset 26."), std::string::npos) << o.out;  // 1.15 mm at 23 px/mm
  EXPECT_NE(o.out.find(", -20."), std::string::npos) << o.out;      // 0.90 mm, up
  EXPECT_NE(o.out.find("move 1.1"), std::string::npos) << o.out;    // the stage move that would centre it
  EXPECT_NE(o.out.find("radius 2"), std::string::npos) << o.out;    // about 1 mm (the finder's own measure)
}

TEST_F(LaserCameraCmd, LookSeesNothingIsExitOne) {
  camera("[co2]\n[co2.sim]\ntray_error_mm = [30.0, 30.0]\nnoise = 0\n");
  const auto o = laser({"look", "co2", "--tray", "example-9"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("no hole"), std::string::npos) << o.err;
}

// Recorded frames need no hardware at all.
TEST_F(LaserCameraCmd, LookOnRecordedFrames) {
  const auto dir = lab("recordings") / "holes";
  fs::create_directories(dir);
  {
    pychron::vision::FrameRecorder recorder(dir, pychron::vision::Provenance::Synthetic, pychron::vision::FinderMode::Hole, 11.5);
    pychron::vision::HoleScene scene;
    scene.hole_mm = {0.5, 0.25};
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(recorder.add(pychron::vision::render(scene, {0, 0}).first.view()));
    ASSERT_TRUE(recorder.finish());
  }
  camera("[co2]\nsource = \"recorded\"\nframes = \"recordings/holes\"\n");
  unplug_the_laser();
  const auto o = laser({"look", "co2"});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("offset 11."), std::string::npos) << o.out;  // 0.5 mm
  EXPECT_NE(o.out.find("move 0.5"), std::string::npos) << o.out;
}

TEST_F(LaserCameraCmd, UsageErrors) {
  EXPECT_EQ(laser({"autocenter"}).code, 2);
  EXPECT_EQ(laser({"autocenter", "co2", "example-9"}).code, 2);
  EXPECT_EQ(laser({"corrections", "co2"}).code, 2);
  EXPECT_EQ(laser({"corrections", "co2", "example-9", "forget"}).code, 2);
  EXPECT_EQ(laser({"corrections", "co2", "example-9", "clear", "5", "6"}).code, 2);
  EXPECT_EQ(laser({"look"}).code, 2);
  EXPECT_EQ(laser({"look", "co2", "extra"}).code, 2);
  EXPECT_EQ(laser({"trays", "--tray", "x"}).code, 2);
}

TEST_F(LaserCmd, TheLabCanBeNamed) {
  fs::rename(dir_ / "lab" / "tray_maps", dir_ / "tray_maps");
  fs::rename(dir_ / "lab" / "stage_calibrations", dir_ / "stage_calibrations");
  EXPECT_NE(laser({"trays"}).out.find("no tray maps"), std::string::npos);
  const auto o = laser({"trays", "--lab", dir_.string()});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("example-9"), std::string::npos) << o.out;
}


// --- cameras, the camera's scale, snapshots (live camera design, 4 to 6) -----

TEST_F(LaserCameraCmd, CamerasListsEveryBackendAndWhatItFinds) {
  // in place of OpenCV's own search, which would open whatever cameras this machine has
  pychron::vision::CameraBackend fake;
  fake.name = "opencv";
  fake.available = true;
  fake.list = [] {
    return std::vector<pychron::vision::CameraFound>{{"opencv", "0", "camera 0 (FAKE)", 1280, 720, 30},
                                            {"opencv", "1", "camera 1 (FAKE)", 640, 480, 0}};
  };
  pychron::vision::register_camera_backend(fake);
  const auto o = laser({"cameras"});
  pychron::vision::unregister_camera_backend("opencv");
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("opencv"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("device = 0"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("1280 x 720"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("30"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("device = 1"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("pylon"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("built without pylon"), std::string::npos) << o.out;
  EXPECT_EQ(laser({"cameras", "co2"}).code, 2);
}

TEST_F(LaserCameraCmd, CameraScaleMeasuresSavesAndIsUsed) {
  const auto o = laser({"camera-scale", "co2", "example-9", "5", "--step", "0.3"}, true);
  ASSERT_EQ(o.code, 0) << o.err << o.out;
  EXPECT_NE(o.out.find("px/mm"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("23."), std::string::npos) << o.out;  // the example's camera: 23 px/mm
  EXPECT_NE(o.out.find("flip_x = false"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("flip_y = true"), std::string::npos) << o.out;
  ASSERT_TRUE(fs::exists(lab("camera_scales") / "co2.toml"));
  // and a centring by it still finds the hole
  const auto centred = laser({"autocenter", "co2", "example-9", "5"}, true);
  ASSERT_EQ(centred.code, 0) << centred.err << centred.out;
  EXPECT_NE(centred.out.find("hole 5: converged"), std::string::npos) << centred.out;

  const auto cleared = laser({"camera-scale", "co2", "clear"});
  ASSERT_EQ(cleared.code, 0) << cleared.err;
  EXPECT_FALSE(fs::exists(lab("camera_scales") / "co2.toml"));
}

// The scale that was measured is the one every command uses. Shown with a
// measurement that is wrong (both axes the wrong way round): a centring by
// it runs away from its hole, and is right again once it is forgotten.
TEST_F(LaserCameraCmd, EveryCommandUsesTheMeasuredScale) {
  fs::create_directories(lab("camera_scales"));
  std::ofstream(lab("camera_scales") / "co2.toml") << "m = [[-0.0434782, 0.0], [0.0, 0.0434782]]\nresidual_mm = 0.0\n";
  const auto shown = laser({"camera-scale", "co2", "show"});
  ASSERT_EQ(shown.code, 0) << shown.err;
  EXPECT_NE(shown.out.find("flip_x = true"), std::string::npos) << shown.out;
  const auto away = laser({"autocenter", "co2", "example-9", "5"}, true);
  EXPECT_EQ(away.code, 1) << "centred by cameras.toml's scale, not the measured one: " << away.out;
  ASSERT_EQ(laser({"camera-scale", "co2", "clear"}).code, 0);
  const auto centred = laser({"autocenter", "co2", "example-9", "5"}, true);
  ASSERT_EQ(centred.code, 0) << centred.err << centred.out;
}

TEST_F(LaserCameraCmd, CameraScaleWithNothingToSeeFailsAndSavesNothing) {
  camera("[co2]\n[co2.sim]\ntray_error_mm = [30.0, 30.0]\nnoise = 0\n");  // the tray is nowhere near
  const auto o = laser({"camera-scale", "co2", "example-9", "5"}, true);
  EXPECT_EQ(o.code, 1);
  EXPECT_NE(o.err.find("nothing to follow"), std::string::npos) << o.err;
  EXPECT_FALSE(fs::exists(lab("camera_scales") / "co2.toml"));
  EXPECT_EQ(laser({"camera-scale", "co2"}, true).code, 2);
  EXPECT_EQ(laser({"camera-scale", "co2", "example-9", "5", "--step", "0"}, true).code, 2);
}

TEST_F(LaserCameraCmd, SnapshotSavesAPicture) {
  const auto o = laser({"snapshot", "co2", "first"}, true);
  ASSERT_EQ(o.code, 0) << o.err << o.out;
  const fs::path file = lab("snapshots") / "co2" / "first.png";
  EXPECT_NE(o.out.find(file.string()), std::string::npos) << o.out;
  ASSERT_TRUE(fs::exists(file));
  // again: beside it, not over it
  const auto again = laser({"snapshot", "co2", "first"}, true);
  ASSERT_EQ(again.code, 0) << again.err;
  EXPECT_TRUE(fs::exists(lab("snapshots") / "co2" / "first-2.png"));
  // a device with no camera has nothing to save
  fs::remove(lab("cameras.toml"));
  const auto none = laser({"snapshot", "co2"}, true);
  EXPECT_EQ(none.code, 1);
  EXPECT_NE(none.err.find("no camera"), std::string::npos) << none.err;
}

}  // namespace
}  // namespace elctl::testing
