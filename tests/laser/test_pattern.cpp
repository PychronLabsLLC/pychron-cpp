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

// A walk much taller than it is wide: legacy's test keeps only points within
// walk_x of the centre, which a draw over the whole box would almost never hit.
TEST(PatternPoints, ANarrowRandomWalkTerminates) {
  Pattern p = of(PatternKind::Random);
  p.npoints = 50;
  p.walk_x = 1e-6;
  p.walk_y = 100;
  const auto points = pattern_points(p, 3);
  ASSERT_EQ(points.size(), 50u);
  for (const auto& q : points) EXPECT_LE(std::hypot(q.x, q.y), 1e-6 + 1e-18);
  p.walk_x = 100;
  p.walk_y = 1e-6;
  const auto flat = pattern_points(p, 3);
  ASSERT_EQ(flat.size(), 50u);
  for (const auto& q : flat) EXPECT_LE(std::abs(q.y), 1e-6);
}

// The count is known without making the points, so a file can be refused for
// it when it is read.
TEST(PatternPoints, TheCountIsKnownWithoutMakingThem) {
  for (auto kind : kKinds) {
    for (int variant = 0; variant < 4; ++variant) {
      Pattern p = of(kind);
      p.nsides = 3 + 5 * variant;
      p.npasses = 1 + variant;
      p.nsteps = 1 + 2 * variant;
      p.step_scalar = 1 + 4 * variant;
      p.npoints = 1 + 7 * variant;
      p.length = 3 + variant;
      p.offset = 0.25 * variant;
      p.dx = 0.1 + 0.35 * variant;
      p.single_pass = variant % 2 == 0;
      EXPECT_EQ(pattern_point_count(p), pattern_points(p, 1).size()) << to_string(kind) << " variant " << variant;
    }
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
                      // a step so fine the raster would be millions of points
                      BadFile{"kind = \"raster\"\nlength = 15\ndx = 0.00000002\n", "dx"},
                      BadFile{"kind = \"raster\"\nlength = 15\ndx = 1e-300\n", "dx"},
                      // every key in range, 11 100 points in all: said when the file is read
                      BadFile{"kind = \"circular_contour\"\nnsteps = 10\niterations = 30\n", "iterations"},
                      BadFile{"kind = \"random\"\nnpoints = 0\n", "npoints"},
                      BadFile{"kind = \"random\"\nseed = -1\n", "seed"},
                      BadFile{"kind = \"linear\"\nnpasses = 0\n", "npasses"},
                      BadFile{"kind = \"linear\"\nlength = nan\n", "length"},
                      BadFile{"kind = \"linear\"\nlength = inf\n", "length"},
                      BadFile{"kind = \"linear\"\nrotation = \"x\"\n", "rotation"},
                      BadFile{"kind = \"linear\"\nnsides = 5\n", "nsides"},  // a polygon's key
                      BadFile{"kind = \"trough\"\nuse_x = 1\n", "use_x"},
                      BadFile{"kind = \"dragonfly\"\nduration = 0\n", "duration"},
                      BadFile{"kind = \"dragonfly\"\nduration = 3601\n", "duration"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nsaturation_threshold = 0\n", "saturation_threshold"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nsaturation_threshold = 1.5\n", "saturation_threshold"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nperimeter_radius = 0\n", "perimeter_radius"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nmax_step = 0\n", "max_step"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\naggressiveness = -1\n", "aggressiveness"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nspiral = \"round\"\n", "spiral"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nspiral = 3\n", "spiral"},
                      BadFile{"kind = \"dragonfly\"\nduration = 10\niterations = 2\n", "iterations"},  // it runs once
                      BadFile{"kind = \"dragonfly\"\nduration = 10\nnsides = 5\n", "nsides"},
                      BadFile{"kind = \"polygon\"\nduration = 10\n", "duration"},  // a dragonfly's key
                      BadFile{"kind = \"arc\"\n", "kind"},
                      BadFile{"kind = 3\n", "kind"},
                      BadFile{"radius = 1\n", "kind"},
                      BadFile{"kind = \"polygon\"\nwobble = 1\n", "wobble"},
                      BadFile{"kind = \"polygon\"\n[nested]\na = 1\n", "nested"},
                      BadFile{"kind = \"polygon\nradius", "bad"}));  // not TOML

// A dragonfly follows the glowing sample for its duration: it is a pattern
// with no path of its own (so it is not among the kinds the point tests loop over).
TEST(PatternFile, ReadsADragonfly) {
  auto bare = Pattern::parse("kind = \"dragonfly\"\nduration = 30\n", "follow");
  ASSERT_TRUE(bare) << bare.error().what;
  EXPECT_EQ(bare->kind, PatternKind::Dragonfly);
  EXPECT_EQ(to_string(bare->kind), "dragonfly");
  EXPECT_TRUE(bare->follows_glow());
  EXPECT_FALSE(of(PatternKind::Polygon).follows_glow());
  EXPECT_DOUBLE_EQ(bare->duration_s, 30);
  // legacy's defaults
  EXPECT_DOUBLE_EQ(bare->velocity, 1.0);
  EXPECT_DOUBLE_EQ(bare->perimeter_radius, 2.5);
  EXPECT_DOUBLE_EQ(bare->saturation_threshold, 0.75);
  EXPECT_DOUBLE_EQ(bare->aggressiveness, 1.0);
  EXPECT_DOUBLE_EQ(bare->move_threshold, 0.033);
  EXPECT_DOUBLE_EQ(bare->max_step, 0.5);
  EXPECT_FALSE(bare->square_spiral);
  EXPECT_DOUBLE_EQ(bare->spiral_base, 0.5);
  EXPECT_DOUBLE_EQ(bare->target_radius, 0.5);

  auto full = Pattern::parse(
      "kind = \"dragonfly\"\nduration = 12.5\nvelocity = 2\nperimeter_radius = 1.5\nsaturation_threshold = 0.6\n"
      "aggressiveness = 0.5\nmove_threshold = 0\nmax_step = 0.25\nspiral = \"square\"\nspiral_base = 0.3\n"
      "target_radius = 0.75\n",
      "f");
  ASSERT_TRUE(full) << full.error().what;
  EXPECT_DOUBLE_EQ(full->duration_s, 12.5);
  EXPECT_DOUBLE_EQ(full->velocity, 2);
  EXPECT_DOUBLE_EQ(full->perimeter_radius, 1.5);
  EXPECT_DOUBLE_EQ(full->saturation_threshold, 0.6);
  EXPECT_DOUBLE_EQ(full->aggressiveness, 0.5);
  EXPECT_DOUBLE_EQ(full->move_threshold, 0);
  EXPECT_DOUBLE_EQ(full->max_step, 0.25);
  EXPECT_TRUE(full->square_spiral);
  EXPECT_DOUBLE_EQ(full->spiral_base, 0.3);
  EXPECT_DOUBLE_EQ(full->target_radius, 0.75);
  EXPECT_TRUE(Pattern::parse("kind = \"dragonfly\"\nduration = 1\nspiral = \"hexagon\"\n", "h"));
  // with no duration of its own it runs for the run's (as legacy pychron)
  auto runs = Pattern::parse("kind = \"dragonfly\"\n", "r");
  ASSERT_TRUE(runs) << runs.error().what;
  EXPECT_DOUBLE_EQ(runs->duration_s, 0);
}

TEST(PatternPoints, ADragonflyHasNoPath) {
  const auto p = Pattern::parse("kind = \"dragonfly\"\nduration = 30\n", "follow");
  ASSERT_TRUE(p);
  EXPECT_TRUE(pattern_points(*p, 0).empty());
  EXPECT_EQ(pattern_point_count(*p), 0u);
  const auto path = pattern_path(*p, 0);
  ASSERT_FALSE(path);
  EXPECT_EQ(path.error().kind, ErrorKind::Config);
  EXPECT_NE(path.error().what.find("follow"), std::string::npos);
}

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

// Writing patterns (laser window design, section 4).

namespace {

constexpr PatternKind kEveryKind[] = {PatternKind::Polygon,      PatternKind::Linear,    PatternKind::CircularContour,
                                      PatternKind::LineSpiral,   PatternKind::SquareSpiral, PatternKind::Random,
                                      PatternKind::Rubberband,   PatternKind::Raster,    PatternKind::Trough,
                                      PatternKind::Dragonfly};

}  // namespace

TEST(PatternWrite, RoundTripsEveryKindsDefaults) {
  for (const PatternKind kind : kEveryKind) {
    Pattern p = Pattern::defaults(kind);
    p.name = "made";
    const std::string text = to_toml(p);
    const auto back = Pattern::parse(text, "made");
    ASSERT_TRUE(back) << to_string(kind) << ": " << back.error().what << "\n" << text;
    EXPECT_EQ(*back, p) << to_string(kind) << "\n" << text;
  }
}

TEST(PatternWrite, RoundTripsEveryField) {
  for (const PatternKind kind : kEveryKind) {
    Pattern p = Pattern::defaults(kind);
    p.name = "made";
    // every field of the kind moved off its default, to a value its range allows
    for (const PatternField& field : pattern_fields(kind)) {
      const double now = *field_value(p, field.key);
      const double next = field.type == PatternField::Type::Flag ? (now != 0 ? 0 : 1)
                          : field.type == PatternField::Type::Whole ? std::min(now + 1, field.high)
                                                                    : std::min(now * 0.7 + 0.0123, field.high);
      ASSERT_TRUE(set_field(p, field.key, next)) << field.key;
    }
    if (kind == PatternKind::Random) p.seed = 18446744073709551615ull >> 1;
    if (kind == PatternKind::Dragonfly) p.square_spiral = true;
    const std::string text = to_toml(p);
    const auto back = Pattern::parse(text, "made");
    ASSERT_TRUE(back) << to_string(kind) << ": " << back.error().what << "\n" << text;
    EXPECT_EQ(*back, p) << to_string(kind) << "\n" << text;
  }
}

TEST(PatternWrite, WritesOnlyTheKindsKeys) {
  Pattern p = Pattern::defaults(PatternKind::Polygon);
  p.length = 7;  // not a polygon's
  const std::string text = to_toml(p);
  EXPECT_NE(text.find("kind = \"polygon\""), std::string::npos) << text;
  EXPECT_NE(text.find("nsides = 6"), std::string::npos) << text;
  EXPECT_EQ(text.find("length"), std::string::npos) << text;
  EXPECT_EQ(text.find("duration"), std::string::npos) << text;
  // a dragonfly has no iterations, and no duration until it is given one
  const std::string fly = to_toml(Pattern::defaults(PatternKind::Dragonfly));
  EXPECT_EQ(fly.find("iterations"), std::string::npos) << fly;
  EXPECT_EQ(fly.find("duration"), std::string::npos) << fly;
  EXPECT_NE(fly.find("spiral = \"hexagon\""), std::string::npos) << fly;
}

TEST(PatternFields, NameTheKindsKeysAndTheirRanges) {
  const auto fields = pattern_fields(PatternKind::Polygon);
  std::vector<std::string> keys;
  for (const auto& f : fields) keys.emplace_back(f.key);
  EXPECT_EQ(keys, (std::vector<std::string>{"velocity", "iterations", "radius", "nsides", "rotation"}));
  EXPECT_EQ(fields[3].type, PatternField::Type::Whole);
  EXPECT_DOUBLE_EQ(fields[3].low, 3);
  Pattern p = Pattern::defaults(PatternKind::Polygon);
  EXPECT_FALSE(set_field(p, "length", 2)) << "not a polygon's";
  EXPECT_FALSE(field_value(p, "length"));
  // a dragonfly has no iterations
  for (const auto& f : pattern_fields(PatternKind::Dragonfly)) EXPECT_NE(f.key, "iterations");
}

TEST(PatternSave, WritesAFileTheLibraryLoads) {
  const auto dir = scratch() / "patterns";  // not there yet
  Pattern p = Pattern::defaults(PatternKind::Linear);
  p.name = "stripe";
  p.length = 3;
  const auto file = save_pattern(dir, p);
  ASSERT_TRUE(file) << file.error().what;
  EXPECT_EQ(*file, dir / "stripe.toml");
  const auto lib = PatternLibrary::load(dir);
  ASSERT_NE(lib.find("stripe"), nullptr);
  EXPECT_EQ(*lib.find("stripe"), p);
  // again, over the first
  p.length = 4;
  ASSERT_TRUE(save_pattern(dir, p));
  EXPECT_DOUBLE_EQ(Pattern::load(dir / "stripe.toml")->length, 4);
  EXPECT_FALSE(fs::exists(dir / "stripe.toml.tmp"));
  fs::remove_all(dir.parent_path());
}

TEST(PatternSave, RefusesANameThatIsNotAFileName) {
  const auto dir = scratch();
  Pattern p = Pattern::defaults(PatternKind::Linear);
  for (const char* name : {"", "..", "../up", "a/b", ".hidden"}) {
    p.name = name;
    const auto file = save_pattern(dir, p);
    ASSERT_FALSE(file) << name;
    EXPECT_EQ(file.error().kind, ErrorKind::Config);
  }
  EXPECT_TRUE(fs::is_empty(dir));
  fs::remove_all(dir);
}

TEST(PatternSave, RefusesAPatternThatCouldNotRun) {
  const auto dir = scratch();
  Pattern p = Pattern::defaults(PatternKind::Polygon);
  p.name = "huge";
  p.nsides = 200;
  p.iterations = 200;  // 40 200 points
  auto file = save_pattern(dir, p);
  ASSERT_FALSE(file);
  EXPECT_NE(file.error().what.find("points"), std::string::npos) << file.error().what;
  p = Pattern::defaults(PatternKind::Polygon);
  p.name = "flat";
  p.radius = 0;
  file = save_pattern(dir, p);
  ASSERT_FALSE(file);
  EXPECT_NE(file.error().what.find("radius"), std::string::npos) << file.error().what;
  EXPECT_TRUE(fs::is_empty(dir));
  fs::remove_all(dir);
}

TEST(PatternLibrary, AFoundPatternOutlivesItsReplacement) {
  PatternLibrary lib;
  Pattern p = Pattern::defaults(PatternKind::Linear);
  p.name = "stripe";
  p.length = 3;
  lib.put(p);
  const std::shared_ptr<const Pattern> held = lib.find("stripe");
  ASSERT_NE(held, nullptr);
  p.length = 9;
  lib.put(p);
  EXPECT_DOUBLE_EQ(held->length, 3) << "whoever is running it keeps the path it started with";
  EXPECT_DOUBLE_EQ(lib.find("stripe")->length, 9);
  EXPECT_EQ(lib.names(), (std::vector<std::string>{"stripe"}));
}

TEST(PatternLibrary, PuttingAPatternClearsItsProblem) {
  const auto dir = scratch();
  std::ofstream(dir / "bad.toml") << "kind = \"polygon\"\nradius = 0\n";
  PatternLibrary lib = PatternLibrary::load(dir);
  ASSERT_EQ(lib.problems().size(), 1u);
  Pattern p = Pattern::defaults(PatternKind::Polygon);
  p.name = "bad";
  lib.put(p);
  EXPECT_TRUE(lib.problems().empty());
  EXPECT_NE(lib.find("bad"), nullptr);
  fs::remove_all(dir);
}
