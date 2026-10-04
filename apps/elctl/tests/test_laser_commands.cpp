// elctl laser: trays, calibrate, goto, on a scratch copy of the example lab
// (its "co2" is a Chromium driver on a simulated transport).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "elctl_fixture.hpp"
#include "pychron/laser/calibration_store.hpp"
#include "pychron/laser/tray_map.hpp"

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
  EXPECT_NE(o.err.find("still moving"), std::string::npos) << o.err;
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

TEST_F(LaserCmd, TheLabCanBeNamed) {
  fs::rename(dir_ / "lab" / "tray_maps", dir_ / "tray_maps");
  fs::rename(dir_ / "lab" / "stage_calibrations", dir_ / "stage_calibrations");
  EXPECT_NE(laser({"trays"}).out.find("no tray maps"), std::string::npos);
  const auto o = laser({"trays", "--lab", dir_.string()});
  ASSERT_EQ(o.code, 0) << o.err;
  EXPECT_NE(o.out.find("example-9"), std::string::npos) << o.out;
}

}  // namespace
}  // namespace elctl::testing
