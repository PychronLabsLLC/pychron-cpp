// Calibrations on disk, one file per device and tray (laser system design,
// section 3.3).

#include "pychron/laser/calibration_store.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::laser;
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

const char* kMap = "circle,1.0\n\n2,3,4,5,1\n1,0,0\n2,0,5\n3,5,0\n4,0,-5\n5,-5,0\n";

class CalibrationStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("pychron_cal_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }

  TrayMap map(const std::string& text = kMap) const {
    auto m = TrayMap::parse(text, "small");
    EXPECT_TRUE(m);
    return *m;
  }
  std::vector<fs::path> files() const {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_ / "cal", ec)) out.push_back(e.path().filename());
    return out;
  }

  fs::path dir_;
  CalibrationStore store_{fs::path()};
  const std::vector<CalibrationPoint> three_{{"1", 10, 20}, {"3", 15, 20}, {"2", 10, 25}};
};

}  // namespace

TEST_F(CalibrationStoreTest, SavesAndLoadsPoints) {
  const CalibrationStore store(dir_ / "cal");  // created on save
  ASSERT_TRUE(store.save(map(), "co2", three_));
  EXPECT_EQ(store.file("co2", "small"), dir_ / "cal" / "co2.small.toml");

  const auto loaded = store.load("co2", "small");
  ASSERT_TRUE(loaded) << loaded.error().what;
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ((*loaded)->device, "co2");
  EXPECT_EQ((*loaded)->tray, "small");
  EXPECT_EQ((*loaded)->tray_sha256, map().sha256());
  EXPECT_EQ((*loaded)->points, three_);

  const std::string text = slurp(store.file("co2", "small"));
  for (const char* key : {"schema_version = 1", "center", "rotation_deg", "scale", "rms_mm", "[[points]]"}) {
    EXPECT_NE(text.find(key), std::string::npos) << key << "\n" << text;
  }

  const auto status = store.status(map(), "co2");
  EXPECT_EQ(status.state, CalibrationState::Ok);
  ASSERT_TRUE(status.solution);
  EXPECT_NEAR(status.solution->transform.cx, 10, 1e-9);
  EXPECT_NEAR(status.solution->transform.cy, 20, 1e-9);
  EXPECT_TRUE(status.why.empty());
}

TEST_F(CalibrationStoreTest, CoordinatesSurviveTheFileExactly) {
  const CalibrationStore store(dir_ / "cal");
  const std::vector<CalibrationPoint> points{{"1", 12.345678901234567, -0.1}, {"3", 17.345678901234567, -0.1}};
  ASSERT_TRUE(store.save(map(), "co2", points));
  const auto loaded = store.load("co2", "small");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->points, points);
}

TEST_F(CalibrationStoreTest, MissingIsNotAnError) {
  const CalibrationStore store(dir_ / "cal");
  const auto loaded = store.load("co2", "small");
  ASSERT_TRUE(loaded);
  EXPECT_FALSE(loaded->has_value());
  const auto status = store.status(map(), "co2");
  EXPECT_EQ(status.state, CalibrationState::Missing);
  EXPECT_FALSE(status.solution);
  EXPECT_NE(status.why.find("small"), std::string::npos);
  EXPECT_NE(status.why.find("co2"), std::string::npos);
}

TEST_F(CalibrationStoreTest, EachDeviceHasItsOwn) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  EXPECT_EQ(store.status(map(), "diode").state, CalibrationState::Missing);
}

TEST_F(CalibrationStoreTest, AChangedMapMakesItStale) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  const auto moved = map("circle,1.0\n\n2,3,4,5,1\n1,0,0\n2,0,5\n3,5.5,0\n4,0,-5\n5,-5,0\n");
  const auto status = store.status(moved, "co2");
  EXPECT_EQ(status.state, CalibrationState::Stale);
  EXPECT_FALSE(status.solution);
  EXPECT_NE(status.why.find("small"), std::string::npos);
}

TEST_F(CalibrationStoreTest, TheSolvedValuesInTheFileAreNotTrusted) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  const auto file = store.file("co2", "small");
  std::string text = slurp(file);
  const auto at = text.find("rotation_deg");
  ASSERT_NE(at, std::string::npos);
  const auto eol = text.find('\n', at);
  text.replace(at, eol - at, "rotation_deg = 45.0");
  std::ofstream(file, std::ios::trunc | std::ios::binary) << text;  // as read: no second CR per line on Windows

  const auto status = store.status(map(), "co2");
  ASSERT_EQ(status.state, CalibrationState::Ok) << status.why;
  EXPECT_NEAR(status.solution->transform.rotation, 0, 1e-9);
}

TEST_F(CalibrationStoreTest, RefusesUnsafeNames) {
  const CalibrationStore store(dir_ / "cal");
  for (const char* name : {"../x", "a/b", "", "a\\b", ".", ".."}) {
    const auto saved = store.save(map(), name, three_);
    ASSERT_FALSE(saved) << name;
    EXPECT_EQ(saved.error().kind, ErrorKind::Config);
    EXPECT_FALSE(store.load(name, "small")) << name;
    EXPECT_FALSE(store.load("co2", name)) << name;
    EXPECT_FALSE(store.clear("co2", name)) << name;
  }
  EXPECT_TRUE(files().empty());
  EXPECT_FALSE(fs::exists(dir_ / "x.small.toml"));
}

TEST_F(CalibrationStoreTest, SaveRefusesPointsThatDoNotSolve) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  const std::string before = slurp(store.file("co2", "small"));
  const std::vector<CalibrationPoint> same_place{{"1", 1, 2}, {"3", 1, 2}};
  const auto saved = store.save(map(), "co2", same_place);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().kind, ErrorKind::Config);
  EXPECT_EQ(slurp(store.file("co2", "small")), before);
  EXPECT_FALSE(store.save(map(), "co2", {}));
}

TEST_F(CalibrationStoreTest, SaveReplacesAndLeavesNothingBehind) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  const std::vector<CalibrationPoint> one{{"1", 1, 2}};
  ASSERT_TRUE(store.save(map(), "co2", one));
  EXPECT_EQ(files(), (std::vector<fs::path>{"co2.small.toml"}));
  const auto loaded = store.load("co2", "small");
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->points, one);
}

TEST_F(CalibrationStoreTest, AHalfWrittenTemporaryIsNeverRead) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  std::ofstream(dir_ / "cal" / "co2.small.toml.tmp") << "schema_version = 1\npoints = [";
  EXPECT_EQ(store.status(map(), "co2").state, CalibrationState::Ok);
}

// The new file is written beside the target and renamed over it: when the
// temporary cannot be written, the save fails and the old file is whole.
TEST_F(CalibrationStoreTest, SaveGoesThroughATemporaryFile) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  const std::string before = slurp(store.file("co2", "small"));
  fs::create_directories(dir_ / "cal" / "co2.small.toml.tmp");  // in the way
  const std::vector<CalibrationPoint> one{{"1", 1, 2}};
  const auto saved = store.save(map(), "co2", one);
  ASSERT_FALSE(saved);
  EXPECT_EQ(saved.error().kind, ErrorKind::Io);
  EXPECT_EQ(slurp(store.file("co2", "small")), before);
}

TEST_F(CalibrationStoreTest, ClearDeletesAndIsIdempotent) {
  const CalibrationStore store(dir_ / "cal");
  ASSERT_TRUE(store.save(map(), "co2", three_));
  ASSERT_TRUE(store.clear("co2", "small"));
  EXPECT_FALSE(fs::exists(store.file("co2", "small")));
  EXPECT_TRUE(store.clear("co2", "small"));
  EXPECT_EQ(store.status(map(), "co2").state, CalibrationState::Missing);
}

TEST_F(CalibrationStoreTest, AFileThatDoesNotParseIsUnsolvable) {
  const CalibrationStore store(dir_ / "cal");
  fs::create_directories(dir_ / "cal");
  struct Case {
    const char* text;
    const char* says;
  };
  for (const Case& c : {Case{"schema_version = 2\ndevice = \"co2\"\ntray = \"small\"\ntray_sha256 = \"x\"\n", "schema_version"},
                        Case{"this is not toml [", "co2.small.toml"},
                        Case{"schema_version = 1\ndevice = \"co2\"\ntray = \"small\"\n", "tray_sha256"},
                        Case{"schema_version = 1\ndevice = \"co2\"\ntray = \"other\"\ntray_sha256 = \"x\"\n", "other"},
                        Case{"schema_version = 1\ndevice = \"co2\"\ntray = \"small\"\ntray_sha256 = \"x\"\n"
                             "[[points]]\nhole = \"1\"\nx = \"left\"\ny = 2\n",
                             "points"}}) {
    std::ofstream(store.file("co2", "small"), std::ios::trunc) << c.text;
    const auto loaded = store.load("co2", "small");
    ASSERT_FALSE(loaded) << c.text;
    EXPECT_EQ(loaded.error().kind, ErrorKind::Config);
    EXPECT_NE(loaded.error().what.find(c.says), std::string::npos) << loaded.error().what;
    const auto status = store.status(map(), "co2");
    EXPECT_EQ(status.state, CalibrationState::Unsolvable);
    EXPECT_FALSE(status.solution);
    EXPECT_FALSE(status.why.empty());
  }
}

TEST_F(CalibrationStoreTest, IntegerCoordinatesInAHandEditedFileAreRead) {
  const CalibrationStore store(dir_ / "cal");
  fs::create_directories(dir_ / "cal");
  std::ofstream(store.file("co2", "small"))
      << "schema_version = 1\ndevice = \"co2\"\ntray = \"small\"\ntray_sha256 = \"" << map().sha256()
      << "\"\n[[points]]\nhole = \"1\"\nx = 10\ny = 20\n";
  const auto status = store.status(map(), "co2");
  ASSERT_EQ(status.state, CalibrationState::Ok) << status.why;
  EXPECT_DOUBLE_EQ(status.solution->transform.cx, 10);
}

TEST(CalibrationStateText, Names) {
  EXPECT_EQ(to_string(CalibrationState::Missing), "not calibrated");
  EXPECT_EQ(to_string(CalibrationState::Stale), "stale");
  EXPECT_EQ(to_string(CalibrationState::Unsolvable), "unsolvable");
  EXPECT_EQ(to_string(CalibrationState::Ok), "ok");
}
