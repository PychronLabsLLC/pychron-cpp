// Per-hole corrections on disk (laser autocenter design, section 5).

#include "pychron/laser/correction_store.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/sha256.hpp"
#include "pychron/laser/calibration_store.hpp"

using namespace pychron;
using namespace pychron::laser;
namespace fs = std::filesystem;

namespace {

const char* kMap = "circle,1.0\n\n2,3,4,5,1\n1,0,0\n2,0,5\n3,5,0\n4,0,-5\n5,-5,0\nA,1,1\n";

std::string slurp(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

class CorrectionStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::random_device rd;
    dir_ = fs::temp_directory_path() / ("pychron_corr_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }
  TrayMap map(const std::string& text = kMap) const {
    auto m = TrayMap::parse(text, "small");
    EXPECT_TRUE(m);
    return *m;
  }
  CorrectionStore store() const { return CorrectionStore(dir_ / "corr"); }

  fs::path dir_;
  const HoleCorrection three_{15.151234567890123, 19.9, 0.011, "2026-10-04T16:20:11Z"};
};

}  // namespace

TEST(Fingerprint, ChangesWithAnyPoint) {
  const std::vector<CalibrationPoint> a{{"1", 10, 20}, {"3", 15, 20}};
  std::vector<CalibrationPoint> b = a;
  EXPECT_EQ(fingerprint(a), fingerprint(b));
  EXPECT_EQ(fingerprint(a).size(), 64u);
  b[1].y = 20.000000001;
  EXPECT_NE(fingerprint(a), fingerprint(b));
  b = a;
  b[1].hole = "2";
  EXPECT_NE(fingerprint(a), fingerprint(b));
  b = a;
  b.push_back({"2", 10, 25});
  EXPECT_NE(fingerprint(a), fingerprint(b));
  EXPECT_NE(fingerprint(a), fingerprint({}));
  // -0.0 and 0.0 are different bits and so different calibrations: exactness, not arithmetic
  EXPECT_NE(fingerprint(std::vector<CalibrationPoint>{{"1", 0.0, 0}}),
            fingerprint(std::vector<CalibrationPoint>{{"1", -0.0, 0}}));
}

// The same on every platform: a lab directory moved between machines keeps
// what was found. The bytes hashed are spelled out (the hole's length and
// text, then the IEEE bits of x and y as 16 hex digits), not a library's
// rendering of a double.
TEST(Fingerprint, IsTheSameEverywhere) {
  const std::vector<CalibrationPoint> points{{"5", 25.0, 25.0}, {"6", 30.0, 25.0}};
  const std::string spelled = "1:5\t4039000000000000\t4039000000000000\n1:6\t403e000000000000\t4039000000000000\n";
  EXPECT_EQ(fingerprint(points), to_hex(pychron::sha256(std::string_view(spelled))));
}

TEST_F(CorrectionStoreTest, TheCalibrationStatusCarriesItsFingerprint) {
  const CalibrationStore calibrations(dir_ / "cal");
  EXPECT_TRUE(calibrations.status(map(), "co2").fingerprint.empty());
  const std::vector<CalibrationPoint> points{{"1", 10, 20}, {"3", 15, 20}};
  ASSERT_TRUE(calibrations.save(map(), "co2", points));
  EXPECT_EQ(calibrations.status(map(), "co2").fingerprint, fingerprint(points));
}

TEST_F(CorrectionStoreTest, PutsAndLoads) {
  const auto s = store();
  EXPECT_EQ(s.file("co2", "small"), dir_ / "corr" / "co2.small.toml");
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "A", {11, 21, 0.02, "2026-10-04T16:21:00Z"}));
  const auto loaded = s.load(map(), "co2", "cal1");
  ASSERT_TRUE(loaded) << loaded.error().what;
  ASSERT_EQ(loaded->size(), 2u);
  EXPECT_EQ(loaded->at("3").x, three_.x);  // exactly
  EXPECT_EQ(loaded->at("3").y, three_.y);
  EXPECT_EQ(loaded->at("3").residual_mm, three_.residual_mm);
  EXPECT_EQ(loaded->at("3").found, three_.found);
  EXPECT_EQ(loaded->at("A").x, 11);

  const std::string text = slurp(s.file("co2", "small"));
  for (const char* key : {"schema_version = 1", "device = 'co2'", "tray = 'small'", "tray_sha256", "calibration = 'cal1'"}) {
    EXPECT_NE(text.find(key), std::string::npos) << key << "\n" << text;
  }
}

TEST_F(CorrectionStoreTest, PutReplacesAHole) {
  const auto s = store();
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", {15.2, 19.8, 0.005, "later"}));
  const auto loaded = s.load(map(), "co2", "cal1");
  ASSERT_TRUE(loaded);
  ASSERT_EQ(loaded->size(), 1u);
  EXPECT_EQ(loaded->at("3").x, 15.2);
  EXPECT_EQ(loaded->at("3").found, "later");
}

TEST_F(CorrectionStoreTest, MissingIsEmpty) {
  const auto loaded = store().load(map(), "co2", "cal1");
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->empty());
}

TEST_F(CorrectionStoreTest, EachDeviceHasItsOwn) {
  const auto s = store();
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  EXPECT_TRUE(s.load(map(), "diode", "cal1")->empty());
}

TEST_F(CorrectionStoreTest, AnotherMapIsIgnoredAndReplacedAtTheNextPut) {
  const auto s = store();
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  const auto edited = map(std::string(kMap) + "B,2,2\n");
  EXPECT_TRUE(s.load(edited, "co2", "cal1")->empty());
  ASSERT_TRUE(s.put(edited, "co2", "cal1", "2", {10, 25, 0, "t"}));
  const auto loaded = s.load(edited, "co2", "cal1");
  ASSERT_EQ(loaded->size(), 1u);  // the old map's hole 3 did not come along
  EXPECT_TRUE(loaded->contains("2"));
  EXPECT_TRUE(s.load(map(), "co2", "cal1")->empty());
}

TEST_F(CorrectionStoreTest, AnotherCalibrationIsIgnoredAndReplaced) {
  const auto s = store();
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  EXPECT_TRUE(s.load(map(), "co2", "cal2")->empty());
  ASSERT_TRUE(s.put(map(), "co2", "cal2", "2", {10, 25, 0, "t"}));
  EXPECT_EQ(s.load(map(), "co2", "cal2")->size(), 1u);
  EXPECT_TRUE(s.load(map(), "co2", "cal1")->empty());
}

TEST_F(CorrectionStoreTest, AHoleNotOnTheMapIsRefusedOrIgnored) {
  const auto s = store();
  auto put = s.put(map(), "co2", "cal1", "99", three_);
  ASSERT_FALSE(put);
  EXPECT_EQ(put.error().kind, ErrorKind::Config);
  EXPECT_FALSE(fs::exists(s.file("co2", "small")));
  // a file edited by hand
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  std::ofstream(s.file("co2", "small"), std::ios::app) << "\n[holes.99]\nx = 1.0\ny = 2.0\n";
  const auto loaded = s.load(map(), "co2", "cal1");
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_EQ(loaded->size(), 1u);
  EXPECT_TRUE(loaded->contains("3"));
}

TEST_F(CorrectionStoreTest, ValuesThatAreNotPositionsAreIgnoredOrRefused) {
  const auto s = store();
  EXPECT_FALSE(s.put(map(), "co2", "cal1", "3", {std::nan(""), 1, 0, "t"}));
  EXPECT_FALSE(s.put(map(), "co2", "cal1", "3", {1, std::numeric_limits<double>::infinity(), 0, "t"}));
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  std::ofstream(s.file("co2", "small"), std::ios::app)
      << "\n[holes.2]\nx = nan\ny = 2.0\n\n[holes.4]\nx = 'left'\ny = 2.0\n\n[holes.5]\nx = 3\ny = 4\n";
  const auto loaded = s.load(map(), "co2", "cal1");
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_FALSE(loaded->contains("2"));
  EXPECT_FALSE(loaded->contains("4"));
  ASSERT_TRUE(loaded->contains("5"));  // integers are numbers
  EXPECT_EQ(loaded->at("5").x, 3);
}

TEST_F(CorrectionStoreTest, RefusesUnsafeNames) {
  const auto s = store();
  for (const char* name : {"../x", "a/b", "", "a\\b", ".", ".."}) {
    EXPECT_FALSE(s.put(map(), name, "cal1", "3", three_)) << name;
    EXPECT_FALSE(s.load(map(), name, "cal1")) << name;
    EXPECT_FALSE(s.clear(name, "small")) << name;
    EXPECT_FALSE(s.clear("co2", name)) << name;
  }
  EXPECT_FALSE(fs::exists(dir_ / "corr"));
}

TEST_F(CorrectionStoreTest, SaveGoesThroughATemporaryFile) {
  const auto s = store();
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  const std::string before = slurp(s.file("co2", "small"));
  fs::create_directories(dir_ / "corr" / "co2.small.toml.tmp");  // in the way
  const auto put = s.put(map(), "co2", "cal1", "2", three_);
  ASSERT_FALSE(put);
  EXPECT_EQ(put.error().kind, ErrorKind::Io);
  EXPECT_EQ(slurp(s.file("co2", "small")), before);
}

TEST_F(CorrectionStoreTest, ClearHoleAndClear) {
  const auto s = store();
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "3", three_));
  ASSERT_TRUE(s.put(map(), "co2", "cal1", "2", three_));
  ASSERT_TRUE(s.clear_hole(map(), "co2", "cal1", "3"));
  auto loaded = s.load(map(), "co2", "cal1");
  EXPECT_EQ(loaded->size(), 1u);
  EXPECT_TRUE(loaded->contains("2"));
  EXPECT_TRUE(s.clear_hole(map(), "co2", "cal1", "3"));  // already gone
  ASSERT_TRUE(s.clear("co2", "small"));
  EXPECT_FALSE(fs::exists(s.file("co2", "small")));
  EXPECT_TRUE(s.clear("co2", "small"));
  EXPECT_TRUE(s.load(map(), "co2", "cal1")->empty());
}

TEST_F(CorrectionStoreTest, AFileThatCannotBeUnderstoodIsAnError) {
  const auto s = store();
  fs::create_directories(dir_ / "corr");
  for (const char* text : {"schema_version = 2\n", "this is [not toml", "device = 'co2'\n"}) {
    std::ofstream(s.file("co2", "small"), std::ios::trunc) << text;
    const auto loaded = s.load(map(), "co2", "cal1");
    ASSERT_FALSE(loaded) << text;
    EXPECT_EQ(loaded.error().kind, ErrorKind::Config);
    EXPECT_NE(loaded.error().what.find("co2.small.toml"), std::string::npos);
    // and a put does not paper over it
    EXPECT_FALSE(s.put(map(), "co2", "cal1", "3", three_));
  }
}
