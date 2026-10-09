// Tray maps in the legacy pychron format (laser system design, section 3.1).

#include "pychron/laser/tray_map.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/sha256.hpp"

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

fs::path data(const std::string& name) { return fs::path(PYCHRON_TEST_DATA_DIR) / "tray_maps" / name; }

std::vector<std::string> ids(const TrayMap& map) {
  std::vector<std::string> out;
  for (const auto& h : map.holes()) out.push_back(h.id);
  return out;
}

fs::path scratch() {
  std::random_device rd;
  auto dir = fs::temp_directory_path() / ("pychron_trays_" + std::to_string(rd()) + std::to_string(rd()));
  fs::create_directories(dir);
  return dir;
}

}  // namespace

TEST(TrayMap, ParsesEveryRowForm) {
  auto map = TrayMap::load(data("small.txt"));
  ASSERT_TRUE(map) << map.error().what;
  EXPECT_EQ(map->name(), "small");
  EXPECT_EQ(map->shape(), HoleShape::Circle);
  EXPECT_DOUBLE_EQ(map->dimension(), 1.0);
  EXPECT_EQ(ids(*map), (std::vector<std::string>{"1", "2", "3", "4", "5", "6", "7", "8", "A"}));
  const Hole* six = map->find("6");
  ASSERT_NE(six, nullptr);
  EXPECT_DOUBLE_EQ(six->x, 7);
  EXPECT_DOUBLE_EQ(six->y, 7);
  ASSERT_NE(map->find("8"), nullptr);
  EXPECT_DOUBLE_EQ(map->find("8")->dimension, 2.5);
  EXPECT_DOUBLE_EQ(map->find("7")->dimension, 1.0);
  const Hole* a = map->find("A");
  ASSERT_NE(a, nullptr);
  EXPECT_DOUBLE_EQ(a->x, 1);
  EXPECT_DOUBLE_EQ(a->y, 2);
  EXPECT_EQ(map->find("nope"), nullptr);
}

TEST(TrayMap, CalibrationHolesComeFromTheHeader) {
  auto map = TrayMap::load(data("small.txt"));
  ASSERT_TRUE(map);
  EXPECT_EQ(map->center_hole(), std::optional<std::string>("1"));
  EXPECT_EQ(map->right_hole(), std::optional<std::string>("3"));

  auto none = TrayMap::parse("circle,1\n\n\n0,0\n", "t");
  ASSERT_TRUE(none) << none.error().what;
  EXPECT_EQ(none->center_hole(), std::nullopt);
  EXPECT_EQ(none->right_hole(), std::nullopt);
}

TEST(TrayMap, CommentsAreSkipped) {
  auto map = TrayMap::parse("# a tray\nsquare,2\n# valid\n1\n# calibration\n\n# holes\n1,2\n# done\n", "t");
  ASSERT_TRUE(map) << map.error().what;
  EXPECT_EQ(map->shape(), HoleShape::Square);
  ASSERT_EQ(map->holes().size(), 1u);
  EXPECT_DOUBLE_EQ(map->holes()[0].y, 2);
}

TEST(TrayMap, BlankLinesAmongTheHolesAreSkipped) {
  auto map = TrayMap::parse("circle,1\n\n\n1,2\n\n   \n3,4\n", "t");
  ASSERT_TRUE(map) << map.error().what;
  EXPECT_EQ(map->holes().size(), 2u);
}

TEST(TrayMap, WindowsFilesLoadTheSame) {
  const std::string unix_text = slurp(data("small.txt"));
  std::string dos = "\xEF\xBB\xBF";
  for (char c : unix_text) {
    if (c == '\n') dos += "  \r\n";
    else dos += c;
  }
  auto a = TrayMap::parse(unix_text, "small");
  auto b = TrayMap::parse(dos, "small");
  ASSERT_TRUE(a);
  ASSERT_TRUE(b) << b.error().what;
  ASSERT_EQ(a->holes().size(), b->holes().size());
  for (std::size_t i = 0; i < a->holes().size(); ++i) {
    EXPECT_EQ(a->holes()[i].id, b->holes()[i].id);
    EXPECT_DOUBLE_EQ(a->holes()[i].x, b->holes()[i].x);
    EXPECT_DOUBLE_EQ(a->holes()[i].y, b->holes()[i].y);
  }
  EXPECT_EQ(a->center_hole(), b->center_hole());
  EXPECT_NE(a->sha256(), b->sha256());
}

struct BadRow {
  const char* text;
  int line;
  friend void PrintTo(const BadRow& row, std::ostream* os) { *os << "line " << row.line; }
};

class TrayMapBad : public ::testing::TestWithParam<BadRow> {};

TEST_P(TrayMapBad, ABadRowNamesItsLine) {
  auto map = TrayMap::parse(GetParam().text, "bad");
  ASSERT_FALSE(map);
  EXPECT_EQ(map.error().kind, ErrorKind::Config);
  EXPECT_NE(map.error().what.find("bad:" + std::to_string(GetParam().line) + ":"), std::string::npos)
      << map.error().what;
}

INSTANTIATE_TEST_SUITE_P(Rows, TrayMapBad,
                         ::testing::Values(BadRow{"circle,1\n\n\n1,x,2\n", 4},              // not a number
                                           BadRow{"circle,1\n\n\n1,2\n1,3,3\n", 5},         // id 1 twice
                                           BadRow{"circle,1\n\n\nnan,1\n", 4},              // not finite
                                           BadRow{"circle,1\n\n\n1e999,1\n", 4},            // overflows
                                           BadRow{"circle,1\n\n\n1,2,3,4,5\n", 4},          // too many fields
                                           BadRow{"circle,1\n\n\n1\n", 4},                  // too few
                                           BadRow{"circle,1\n\n\n1,2,r0\n", 4},             // no size
                                           BadRow{"triangle,1\n\n\n1,2\n", 1},              // shape
                                           BadRow{"circle,-1\n\n\n1,2\n", 1},               // dimension
                                           BadRow{"circle,1\n\n\n1,2,r\n", 4}));            // no size

// Legacy pychron never checked the calibration line, so old maps have all
// sorts in it. One that is not five holes of the map means "none", and the
// map still loads.
TEST(TrayMap, ACalibrationLineThatIsNotFiveHolesMeansNone) {
  for (const char* text : {"circle,1\n\n1,2,3\n0,0\n1,0\n2,0\n", "circle,1\n\n9,9,9,9,9\n0,0\n",
                           "circle,1\n\n1,,1,1,1\n0,0\n"}) {
    auto map = TrayMap::parse(text, "old");
    ASSERT_TRUE(map) << text << ": " << map.error().what;
    EXPECT_EQ(map->center_hole(), std::nullopt);
    EXPECT_EQ(map->right_hole(), std::nullopt);
  }
}

// As legacy did: a # ends a line anywhere, not only at its start.
TEST(TrayMap, CommentsMayFollowData) {
  auto map = TrayMap::parse("circle,1.0 # mm\n1,2 # valid\n2,2,2,2,1 # n e s w c\n0,0 # first\n5,0\n", "t");
  ASSERT_TRUE(map) << map.error().what;
  EXPECT_DOUBLE_EQ(map->dimension(), 1.0);
  ASSERT_EQ(map->holes().size(), 2u);
  EXPECT_EQ(map->center_hole(), std::optional<std::string>("1"));
}

TEST(TrayMap, AFileWithoutItsHeaderIsAnError) {
  for (const char* text : {"", "# nothing\n", "circle,1\n", "circle,1\n\n"}) {
    auto map = TrayMap::parse(text, "short");
    ASSERT_FALSE(map) << text;
    EXPECT_EQ(map.error().kind, ErrorKind::Config);
    EXPECT_NE(map.error().what.find("short"), std::string::npos);
  }
}

TEST(TrayMap, AMapWithNoHolesIsAnError) {
  EXPECT_FALSE(TrayMap::parse("circle,1\n\n\n", "empty"));
}

TEST(TrayMap, ShaIsOfTheBytes) {
  const std::string text = slurp(data("small.txt"));
  auto map = TrayMap::parse(text, "small");
  ASSERT_TRUE(map);
  const auto digest = pychron::sha256(std::string_view(text));
  EXPECT_EQ(map->sha256(), to_hex(digest));
}

TEST(TrayMap, TheLegacy221MapLoads) {
  auto map = TrayMap::load(data("221-hole.txt"));
  ASSERT_TRUE(map) << map.error().what;
  EXPECT_EQ(map->name(), "221-hole");
  EXPECT_EQ(map->holes().size(), 221u);
  ASSERT_NE(map->find("1"), nullptr);
  EXPECT_DOUBLE_EQ(map->find("1")->x, -3.9878);
  EXPECT_DOUBLE_EQ(map->find("1")->y, 15.9512);
  ASSERT_NE(map->find("221"), nullptr);
  EXPECT_EQ(map->center_hole(), std::optional<std::string>("111"));
  EXPECT_EQ(map->right_hole(), std::optional<std::string>("119"));
  EXPECT_DOUBLE_EQ(map->find("111")->x, 0);
  EXPECT_DOUBLE_EQ(map->find("111")->y, 0);
}

TEST(TrayMap, AMissingFileIsAConfigError) {
  auto map = TrayMap::load(data("no-such.txt"));
  ASSERT_FALSE(map);
  EXPECT_EQ(map.error().kind, ErrorKind::Config);
}

TEST(TrayLibrary, LoadsADirectoryAndReportsWhatDidNot) {
  const auto dir = scratch();
  fs::copy_file(data("small.txt"), dir / "good.txt");
  std::ofstream(dir / "bad.txt") << "circle,1\n\n\n1,x\n";
  std::ofstream(dir / "notes.md") << "not a tray\n";
  // On network and exFAT volumes. Written by length: << would stop at the first byte, a NUL.
  const std::string_view fork("\x00\x05\x16\x07 macOS resource fork", 24);
  std::ofstream(dir / "._good.txt", std::ios::binary).write(fork.data(), static_cast<std::streamsize>(fork.size()));
  fs::create_directories(dir / "folder.txt");

  const auto lib = TrayLibrary::load(dir);
  EXPECT_EQ(lib.names(), (std::vector<std::string>{"good"}));
  ASSERT_NE(lib.find("good"), nullptr);
  EXPECT_EQ(lib.find("bad"), nullptr);
  ASSERT_EQ(lib.problems().size(), 1u);
  EXPECT_NE(lib.problems()[0].find("bad"), std::string::npos);
  fs::remove_all(dir);
}

TEST(TrayLibrary, AMissingDirectoryIsEmpty) {
  const auto lib = TrayLibrary::load(fs::temp_directory_path() / "pychron_no_such_tray_dir");
  EXPECT_TRUE(lib.names().empty());
  EXPECT_TRUE(lib.problems().empty());
}
