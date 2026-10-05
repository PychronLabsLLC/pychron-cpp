// Laser patterns: the files, and the points each kind generates (laser
// patterns design, sections 3 to 5). The geometry is legacy pychron's.

#include "pychron/laser/pattern.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::laser;
namespace fs = std::filesystem;

namespace {

struct XY {
  double x, y;
};

::testing::AssertionResult are(const std::vector<StageXY>& got, const std::vector<XY>& want) {
  if (got.size() != want.size()) {
    return ::testing::AssertionFailure() << got.size() << " points, expected " << want.size();
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (std::abs(got[i].x - want[i].x) > 1e-9 || std::abs(got[i].y - want[i].y) > 1e-9) {
      return ::testing::AssertionFailure() << "point " << i << " is (" << got[i].x << ", " << got[i].y << "), expected ("
                                           << want[i].x << ", " << want[i].y << ")";
    }
  }
  return ::testing::AssertionSuccess();
}

Pattern of(PatternKind kind) { return Pattern::defaults(kind); }

const PatternKind kKinds[] = {PatternKind::Polygon,      PatternKind::Linear, PatternKind::CircularContour,
                              PatternKind::LineSpiral,   PatternKind::SquareSpiral, PatternKind::Random,
                              PatternKind::Rubberband,   PatternKind::Raster, PatternKind::Trough};

fs::path scratch() {
  std::random_device rd;
  auto dir = fs::temp_directory_path() / ("pychron_patterns_" + std::to_string(rd()) + std::to_string(rd()));
  fs::create_directories(dir);
  return dir;
}

}  // namespace

TEST(PatternPoints, Polygon) {
  Pattern p = of(PatternKind::Polygon);
  p.radius = 1;
  p.nsides = 4;
  EXPECT_TRUE(are(pattern_points(p, 0), {{1, 0}, {0, 1}, {-1, 0}, {0, -1}, {1, 0}}));
  p.rotation = 45;
  const double h = std::sqrt(0.5);
  EXPECT_TRUE(are(pattern_points(p, 0), {{h, h}, {-h, h}, {-h, -h}, {h, -h}, {h, h}}));
}

TEST(PatternPoints, Linear) {
  Pattern p = of(PatternKind::Linear);
  p.length = 2;
  p.npasses = 3;
  EXPECT_TRUE(are(pattern_points(p, 0), {{0, 0}, {2, 0}, {2, 0}, {0, 0}, {0, 0}, {2, 0}}));
  p.npasses = 1;
  p.rotation = 90;
  EXPECT_TRUE(are(pattern_points(p, 0), {{0, 0}, {0, 2}}));
}

TEST(PatternPoints, CircularContour) {
  Pattern p = of(PatternKind::CircularContour);
  p.radius = 1;
  p.nsteps = 2;
  p.percent_change = 0.5;
  const auto points = pattern_points(p, 0);
  ASSERT_EQ(points.size(), 74u);  // two rings of 37: 0 to 360 degrees by 10
  EXPECT_NEAR(points[0].x, 1, 1e-9);
  EXPECT_NEAR(points[0].y, 0, 1e-9);
  EXPECT_NEAR(points[9].x, 0, 1e-9);
  EXPECT_NEAR(points[9].y, 1, 1e-9);
  EXPECT_NEAR(points[36].x, 1, 1e-9);
  EXPECT_NEAR(points[36].y, 0, 1e-9);
  EXPECT_NEAR(points[37].x, 1.5, 1e-9);
  for (std::size_t i = 37; i < 74; ++i) EXPECT_NEAR(std::hypot(points[i].x, points[i].y), 1.5, 1e-9);
}

TEST(PatternPoints, LineSpiral) {
  Pattern p = of(PatternKind::LineSpiral);
  p.radius = 1;
  p.nsteps = 2;
  p.percent_change = 0.5;
  p.step_scalar = 5;
  const auto points = pattern_points(p, 0);
  ASSERT_EQ(points.size(), 11u);  // 5 angles less the 360, then 7
  EXPECT_NEAR(points.front().x, 1, 1e-9);
  EXPECT_NEAR(points.front().y, 0, 1e-9);
  EXPECT_NEAR(points[1].x, 0, 1e-9);            // 90 degrees of the first turn
  EXPECT_NEAR(points[1].y, 1 + 0.25 * 0.5, 1e-9);
  EXPECT_NEAR(points.back().x, 2, 1e-9);        // R (1 + nsteps * percent_change)
  EXPECT_NEAR(points.back().y, 0, 1e-9);
  double last = 0;
  for (const auto& q : points) {
    const double r = std::hypot(q.x, q.y);
    EXPECT_GE(r, last - 1e-9);
    last = r;
  }
}

TEST(PatternPoints, SquareSpiral) {
  Pattern p = of(PatternKind::SquareSpiral);
  p.radius = 1;
  p.nsteps = 1;
  p.percent_change = 1;
  EXPECT_TRUE(are(pattern_points(p, 0), {{1, 0}, {1, 2}, {-2, 2}, {-2, -2}, {3, -2}}));
  p.nsteps = 2;
  EXPECT_EQ(pattern_points(p, 0).size(), 9u);
}

TEST(PatternPoints, Rubberband) {
  Pattern p = of(PatternKind::Rubberband);
  p.length = 4;
  p.offset = 1;
  EXPECT_TRUE(are(pattern_points(p, 0), {{-1, 1}, {5, 1}, {5, -1}, {-1, -1}, {-1, 1}}));
  p.rotation = 180;
  EXPECT_TRUE(are(pattern_points(p, 0), {{1, -1}, {-5, -1}, {-5, 1}, {1, 1}, {1, -1}}));
}

// Legacy fits the step to the box: 6 mm at dx = 1 becomes 7 steps of 6/7.
TEST(PatternPoints, RasterAdjustsItsStep) {
  Pattern p = of(PatternKind::Raster);
  p.length = 4;
  p.offset = 1;
  p.dx = 1;
  auto points = pattern_points(p, 0);
  ASSERT_EQ(points.size(), 8u);
  for (std::size_t i = 0; i < points.size(); ++i) {
    EXPECT_NEAR(points[i].x, -1 + 6.0 / 7.0 * static_cast<double>(i), 1e-9) << i;
    EXPECT_NEAR(points[i].y, i % 2 == 0 ? 1 : -1, 1e-9) << i;
  }
  EXPECT_NEAR(points.back().x, 5, 1e-9);  // it ends on the far edge

  p.single_pass = false;
  points = pattern_points(p, 0);
  ASSERT_EQ(points.size(), 17u);  // out, back, and to the first corner
  EXPECT_NEAR(points[8].x, 5, 1e-9);
  EXPECT_NEAR(points[8].y, 1, 1e-9);
  EXPECT_NEAR(points[15].x, -1, 1e-9);
  EXPECT_NEAR(points[16].x, -1, 1e-9);
  EXPECT_NEAR(points[16].y, 1, 1e-9);
}

TEST(PatternPoints, RasterStepsNeverExceedWhatWasAsked) {
  for (double dx : {0.1, 0.3, 0.5, 0.7, 1.0, 2.5, 6.0}) {
    Pattern p = of(PatternKind::Raster);
    p.length = 4;
    p.offset = 1;
    p.dx = dx;
    const auto points = pattern_points(p, 0);
    ASSERT_GE(points.size(), 2u) << dx;
    EXPECT_LE(points[1].x - points[0].x, dx + 1e-9) << dx;
    EXPECT_NEAR(points.back().x, 5, 1e-9) << dx;
    EXPECT_EQ(points.size() % 2, 0u) << dx;  // ends on the bottom edge
  }
}

TEST(PatternPoints, Trough) {
  Pattern p = of(PatternKind::Trough);
  p.length = 3;
  p.width = 2;
  EXPECT_TRUE(are(pattern_points(p, 0), {{0, 0}, {3, 0}, {0, -2}, {3, -2}, {0, 0}}));
  p.use_x = false;
  EXPECT_TRUE(are(pattern_points(p, 0), {{0, 0}, {3, 0}, {3, -2}, {0, -2}, {0, 0}}));
}

TEST(PatternPoints, RandomIsSeededAndBounded) {
  Pattern p = of(PatternKind::Random);
  p.npoints = 50;
  p.walk_x = 2;
  p.walk_y = 3;
  const auto a = pattern_points(p, 7);
  const auto b = pattern_points(p, 7);
  const auto c = pattern_points(p, 8);
  ASSERT_EQ(a.size(), 50u);
  bool differs = false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].x, b[i].x);
    EXPECT_EQ(a[i].y, b[i].y);
    differs = differs || a[i].x != c[i].x;
    EXPECT_LE(std::abs(a[i].x), 2);
    EXPECT_LE(std::abs(a[i].y), 3);
    EXPECT_LE(std::hypot(a[i].x, a[i].y), 2 + 1e-12);  // legacy's circle test, on walk_x
  }
  EXPECT_TRUE(differs);
}

// The same walk on every compiler: the generator and the mapping to [0, 1)
// are spelled out, not left to a standard library's distribution.
TEST(PatternPoints, RandomMappingIsFixed) {
  Pattern p = of(PatternKind::Random);
  p.npoints = 1;
  std::mt19937_64 rng(1);
  const auto unit = [&rng] { return static_cast<double>(rng() >> 11) * 0x1.0p-53; };
  double x = 0, y = 0;
  do {
    x = unit() * 2 - 1;
    y = unit() * 2 - 1;
  } while (std::hypot(x, y) > 1);
  const auto points = pattern_points(p, 1);
  ASSERT_EQ(points.size(), 1u);
  EXPECT_EQ(points[0].x, x);
  EXPECT_EQ(points[0].y, y);
}

TEST(PatternPoints, EveryKindsDefaultsGiveFinitePoints) {
  for (auto kind : kKinds) {
    const auto points = pattern_points(of(kind), 3);
    EXPECT_GE(points.size(), 2u) << to_string(kind);
    for (const auto& q : points) EXPECT_TRUE(std::isfinite(q.x) && std::isfinite(q.y)) << to_string(kind);
  }
}

TEST(PatternPath, RepeatsAndReturnsToTheCentre) {
  Pattern p = of(PatternKind::Polygon);
  p.nsides = 4;
  p.iterations = 3;
  const auto path = pattern_path(p, 0);
  ASSERT_TRUE(path) << path.error().what;
  ASSERT_EQ(path->size(), 16u);
  EXPECT_DOUBLE_EQ(path->back().x, 0);
  EXPECT_DOUBLE_EQ(path->back().y, 0);
  EXPECT_NEAR((*path)[5].x, (*path)[0].x, 1e-12);
}

TEST(PatternPath, EachIterationOfARandomWalkIsNew) {
  Pattern p = of(PatternKind::Random);
  p.npoints = 5;
  p.iterations = 2;
  const auto path = pattern_path(p, 4);
  ASSERT_TRUE(path);
  ASSERT_EQ(path->size(), 11u);
  EXPECT_NE((*path)[0].x, (*path)[5].x);
  const auto again = pattern_path(p, 4);
  EXPECT_EQ((*path)[7].y, (*again)[7].y);
}

TEST(PatternPath, TooManyPointsIsRefused) {
  Pattern p = of(PatternKind::CircularContour);
  p.name = "huge";
  p.nsteps = 10;
  p.iterations = 200;  // 370 x 200
  const auto path = pattern_path(p, 0);
  ASSERT_FALSE(path);
  EXPECT_EQ(path.error().kind, ErrorKind::Config);
  EXPECT_NE(path.error().what.find("huge"), std::string::npos);
  EXPECT_NE(path.error().what.find("10000"), std::string::npos);
}

TEST(PathLength, SumsTheSegmentsFromTheCentre) {
  Pattern p = of(PatternKind::Rubberband);
  p.length = 4;
  p.offset = 1;
  EXPECT_NEAR(path_length(pattern_points(p, 0)), std::sqrt(2.0) + 6 + 2 + 6 + 2, 1e-9);
  EXPECT_DOUBLE_EQ(path_length({}), 0);
}

TEST(PatternFile, ReadsEveryKindWithItsDefaults) {
  for (auto kind : kKinds) {
    const std::string text = "kind = \"" + std::string(to_string(kind)) + "\"\n";
    auto p = Pattern::parse(text, "p");
    ASSERT_TRUE(p) << to_string(kind) << ": " << p.error().what;
    Pattern want = Pattern::defaults(kind);
    want.name = "p";
    EXPECT_EQ(*p, want) << to_string(kind);
    EXPECT_DOUBLE_EQ(p->velocity, 1.0);
    EXPECT_EQ(p->iterations, 1);
  }
  // the defaults are legacy's
  EXPECT_DOUBLE_EQ(of(PatternKind::Polygon).radius, 0.5);
  EXPECT_EQ(of(PatternKind::Polygon).nsides, 6);
  EXPECT_DOUBLE_EQ(of(PatternKind::Linear).length, 1);
  EXPECT_DOUBLE_EQ(of(PatternKind::CircularContour).radius, 0.1);
  EXPECT_EQ(of(PatternKind::CircularContour).nsteps, 2);
  EXPECT_DOUBLE_EQ(of(PatternKind::CircularContour).percent_change, 0.8);
  EXPECT_EQ(of(PatternKind::LineSpiral).step_scalar, 5);
  EXPECT_DOUBLE_EQ(of(PatternKind::SquareSpiral).radius, 0.1);
  EXPECT_EQ(of(PatternKind::Random).npoints, 10);
  EXPECT_DOUBLE_EQ(of(PatternKind::Rubberband).length, 15);
  EXPECT_DOUBLE_EQ(of(PatternKind::Rubberband).offset, 0);
  EXPECT_DOUBLE_EQ(of(PatternKind::Raster).dx, 0.5);
  EXPECT_TRUE(of(PatternKind::Raster).single_pass);
  EXPECT_DOUBLE_EQ(of(PatternKind::Trough).length, 10);
  EXPECT_DOUBLE_EQ(of(PatternKind::Trough).width, 10);
  EXPECT_TRUE(of(PatternKind::Trough).use_x);
}

TEST(PatternFile, ReadsItsKeys) {
  auto polygon = Pattern::parse("kind = \"polygon\"\nvelocity = 2.5\niterations = 4\nradius = 1.25\nnsides = 7\nrotation = -30\n",
                                "hept");
  ASSERT_TRUE(polygon) << polygon.error().what;
  EXPECT_EQ(polygon->name, "hept");
  EXPECT_EQ(polygon->kind, PatternKind::Polygon);
  EXPECT_DOUBLE_EQ(polygon->velocity, 2.5);
  EXPECT_EQ(polygon->iterations, 4);
  EXPECT_DOUBLE_EQ(polygon->radius, 1.25);
  EXPECT_EQ(polygon->nsides, 7);
  EXPECT_DOUBLE_EQ(polygon->rotation, -30);

  auto raster = Pattern::parse(
      "kind = \"raster\"\nlength = 9\noffset = 0.5\nrotation = 15\ndx = 0.25\nsingle_pass = false\nvelocity = 1\n", "r");
  ASSERT_TRUE(raster) << raster.error().what;
  EXPECT_DOUBLE_EQ(raster->length, 9);  // an integer is a number
  EXPECT_DOUBLE_EQ(raster->offset, 0.5);
  EXPECT_DOUBLE_EQ(raster->dx, 0.25);
  EXPECT_FALSE(raster->single_pass);

  auto walk = Pattern::parse("kind = \"random\"\nseed = 42\nwalk_x = 0.5\nwalk_y = 0.25\nnpoints = 3\n", "w");
  ASSERT_TRUE(walk) << walk.error().what;
  EXPECT_EQ(walk->seed, std::optional<std::uint64_t>(42));
  EXPECT_EQ(of(PatternKind::Random).seed, std::nullopt);
}

struct BadFile {
  const char* text;
  const char* key;
  friend void PrintTo(const BadFile& b, std::ostream* os) { *os << b.key; }
};

class PatternFileBad : public ::testing::TestWithParam<BadFile> {};

TEST_P(PatternFileBad, RefusesBadValues) {
  auto p = Pattern::parse(GetParam().text, "bad");
  ASSERT_FALSE(p) << GetParam().text;
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
  EXPECT_EQ(p.error().what.rfind("bad: ", 0), 0u) << p.error().what;
  EXPECT_NE(p.error().what.find(GetParam().key), std::string::npos) << p.error().what;
}

INSTANTIATE_TEST_SUITE_P(
    Values, PatternFileBad,
    ::testing::Values(BadFile{"kind = \"polygon\"\nradius = 0\n", "radius"},
                      BadFile{"kind = \"polygon\"\nradius = -1\n", "radius"},
                      BadFile{"kind = \"polygon\"\nnsides = 2\n", "nsides"},
                      BadFile{"kind = \"polygon\"\nnsides = 201\n", "nsides"},
                      BadFile{"kind = \"polygon\"\nnsides = 4.5\n", "nsides"},
                      BadFile{"kind = \"polygon\"\nvelocity = 0\n", "velocity"},
                      BadFile{"kind = \"polygon\"\nvelocity = -2\n", "velocity"},
                      BadFile{"kind = \"polygon\"\niterations = 0\n", "iterations"},
                      BadFile{"kind = \"polygon\"\niterations = 201\n", "iterations"},
                      BadFile{"kind = \"raster\"\ndx = 0\n", "dx"},
                      BadFile{"kind = \"raster\"\nlength = 2\noffset = 0\ndx = 3\n", "dx"},  // wider than its box
                      BadFile{"kind = \"random\"\nnpoints = 0\n", "npoints"},
                      BadFile{"kind = \"random\"\nseed = -1\n", "seed"},
                      BadFile{"kind = \"linear\"\nnpasses = 0\n", "npasses"},
                      BadFile{"kind = \"linear\"\nlength = nan\n", "length"},
                      BadFile{"kind = \"linear\"\nlength = inf\n", "length"},
                      BadFile{"kind = \"linear\"\nrotation = \"x\"\n", "rotation"},
                      BadFile{"kind = \"linear\"\nnsides = 5\n", "nsides"},  // a polygon's key
                      BadFile{"kind = \"trough\"\nuse_x = 1\n", "use_x"},
                      BadFile{"kind = \"arc\"\n", "kind"},
                      BadFile{"kind = 3\n", "kind"},
                      BadFile{"radius = 1\n", "kind"},
                      BadFile{"kind = \"polygon\"\nwobble = 1\n", "wobble"},
                      BadFile{"kind = \"polygon\"\n[nested]\na = 1\n", "nested"},
                      BadFile{"kind = \"polygon\nradius", "bad"}));  // not TOML

TEST(PatternFile, LoadsFromAFileNamedAfterItsStem) {
  const auto dir = scratch();
  std::ofstream(dir / "hexagon.toml") << "kind = \"polygon\"\nnsides = 6\n";
  auto p = Pattern::load(dir / "hexagon.toml");
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_EQ(p->name, "hexagon");
  EXPECT_FALSE(Pattern::load(dir / "missing.toml"));
  fs::remove_all(dir);
}

TEST(PatternLibrary, LoadsADirectoryAndReportsWhatDidNot) {
  const auto dir = scratch();
  std::ofstream(dir / "good.toml") << "kind = \"linear\"\nlength = 2\n";
  std::ofstream(dir / "bad.toml") << "kind = \"polygon\"\nradius = 0\n";
  std::ofstream(dir / "._good.toml") << "junk";
  std::ofstream(dir / "notes.txt") << "kind = \"polygon\"\n";
  fs::create_directories(dir / "folder.toml");

  const auto lib = PatternLibrary::load(dir);
  EXPECT_EQ(lib.names(), (std::vector<std::string>{"good"}));
  ASSERT_NE(lib.find("good"), nullptr);
  EXPECT_DOUBLE_EQ(lib.find("good")->length, 2);
  EXPECT_EQ(lib.find("bad"), nullptr);
  ASSERT_EQ(lib.problems().size(), 1u);
  EXPECT_EQ(lib.problems()[0].rfind("bad: ", 0), 0u) << lib.problems()[0];
  fs::remove_all(dir);

  const auto none = PatternLibrary::load(dir / "nowhere");
  EXPECT_TRUE(none.names().empty());
  EXPECT_TRUE(none.problems().empty());
}
