// Stage calibration from (hole, stage position) points (laser system design,
// section 3.2).

#include "pychron/laser/calibration.hpp"

#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace pychron;
using namespace pychron::laser;

namespace {

constexpr double kDeg = std::numbers::pi / 180.0;

// Holes 1 (0,0), 2 (0,5), 3 (5,0), 4 (0,-5), 5 (-5,0), and four more.
TrayMap small() {
  auto map = TrayMap::load(std::filesystem::path(PYCHRON_TEST_DATA_DIR) / "tray_maps" / "small.txt");
  EXPECT_TRUE(map);
  return *map;
}

// Where `t` puts `hole`, moved by (dx, dy).
CalibrationPoint at(const TrayMap& map, const Transform& t, const std::string& hole, double dx = 0, double dy = 0) {
  const Hole* h = map.find(hole);
  EXPECT_NE(h, nullptr) << hole;
  const auto s = t.to_stage(h->x, h->y);
  return {hole, s.x + dx, s.y + dy};
}

}  // namespace

TEST(Solve, NoPointsIsNotCalibrated) {
  const auto map = small();
  const auto s = solve(map, {});
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().kind, ErrorKind::Config);
  EXPECT_NE(s.error().what.find("not calibrated"), std::string::npos);
}

TEST(Solve, OnePointIsAShift) {
  const auto map = small();
  const std::vector<CalibrationPoint> points{{"1", 12, 8}};
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_DOUBLE_EQ(s->transform.cx, 12);
  EXPECT_DOUBLE_EQ(s->transform.cy, 8);
  EXPECT_DOUBLE_EQ(s->transform.rotation, 0);
  EXPECT_DOUBLE_EQ(s->transform.scale, 1);
  EXPECT_DOUBLE_EQ(s->rms_mm, 0);
  EXPECT_EQ(s->points, 1u);
}

TEST(Solve, OnePointOffTheOriginStillLandsOnItsHole) {
  const auto map = small();
  const std::vector<CalibrationPoint> points{{"3", 20, 8}};  // hole 3 is at (5, 0)
  const auto s = solve(map, points);
  ASSERT_TRUE(s);
  EXPECT_DOUBLE_EQ(s->transform.cx, 15);
  const auto p = s->transform.to_stage(5, 0);
  EXPECT_DOUBLE_EQ(p.x, 20);
  EXPECT_DOUBLE_EQ(p.y, 8);
}

TEST(Solve, TwoPointsGiveShiftAndRotation) {
  const auto map = small();
  const Transform truth{12, 8, 3 * kDeg, 1};
  const std::vector<CalibrationPoint> points{at(map, truth, "1"), at(map, truth, "3")};
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_NEAR(s->transform.rotation, 3 * kDeg, 1e-9);
  EXPECT_DOUBLE_EQ(s->transform.scale, 1);
  EXPECT_NEAR(s->rms_mm, 0, 1e-9);
  for (const auto& h : map.holes()) {
    const auto want = truth.to_stage(h.x, h.y);
    const auto got = s->transform.to_stage(h.x, h.y);
    EXPECT_NEAR(got.x, want.x, 1e-9) << h.id;
    EXPECT_NEAR(got.y, want.y, 1e-9) << h.id;
  }
}

TEST(Solve, TwoPointsNotThroughTheOrigin) {
  const auto map = small();
  const Transform truth{-3, 40, -100 * kDeg, 1};
  const std::vector<CalibrationPoint> points{at(map, truth, "2"), at(map, truth, "A")};
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_NEAR(s->transform.rotation, -100 * kDeg, 1e-9);
  EXPECT_NEAR(s->transform.cx, -3, 1e-9);
  EXPECT_NEAR(s->transform.cy, 40, 1e-9);
}

TEST(Solve, TwoPointsAtTheWrongDistanceShowInRms) {
  const auto map = small();
  const std::vector<CalibrationPoint> points{{"1", 12, 8}, {"3", 17.2, 8}};  // 5 mm apart on the map
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_DOUBLE_EQ(s->transform.scale, 1);
  EXPECT_NEAR(s->rms_mm, 0.2, 1e-9);
  const auto first = s->transform.to_stage(0, 0);
  EXPECT_NEAR(first.x, 12, 1e-12);
  EXPECT_NEAR(first.y, 8, 1e-12);
}

TEST(Solve, ManyPointsAreFittedWithScale) {
  const auto map = small();
  const Transform truth{3, -4, -7 * kDeg, 1.01};
  const std::vector<CalibrationPoint> points{at(map, truth, "1", 0.01, 0), at(map, truth, "2", -0.01, 0.005),
                                             at(map, truth, "3", 0, -0.01), at(map, truth, "4", 0.005, 0.01),
                                             at(map, truth, "5", -0.005, -0.005)};
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_NEAR(s->transform.rotation, -7 * kDeg, 1e-3);
  EXPECT_NEAR(s->transform.scale, 1.01, 1e-3);
  EXPECT_NEAR(s->transform.cx, 3, 0.02);
  EXPECT_NEAR(s->transform.cy, -4, 0.02);
  EXPECT_GT(s->rms_mm, 0);
  EXPECT_LT(s->rms_mm, 0.02);
  EXPECT_EQ(s->points, 5u);
}

TEST(Solve, ExactPointsAreFittedExactly) {
  const auto map = small();
  const Transform truth{50, 60, 170 * kDeg, 0.99};
  std::vector<CalibrationPoint> points;
  for (const char* h : {"2", "3", "6", "A"}) points.push_back(at(map, truth, h));
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_NEAR(s->transform.rotation, 170 * kDeg, 1e-9);
  EXPECT_NEAR(s->transform.scale, 0.99, 1e-9);
  EXPECT_NEAR(s->transform.cx, 50, 1e-9);
  EXPECT_NEAR(s->transform.cy, 60, 1e-9);
  EXPECT_NEAR(s->rms_mm, 0, 1e-9);
}

TEST(Transform, RoundTrips) {
  const Transform t{3, -4, 30 * kDeg, 1.015};
  for (const auto& [x, y] : std::vector<std::pair<double, double>>{{0, 0}, {5, -7}, {-15.9512, 3.9878}}) {
    const auto s = t.to_stage(x, y);
    const auto m = t.to_map(s.x, s.y);
    EXPECT_NEAR(m.x, x, 1e-12);
    EXPECT_NEAR(m.y, y, 1e-12);
  }
}

TEST(Transform, RotatesCounterClockwise) {
  const Transform t{10, 20, 90 * kDeg, 1};
  const auto s = t.to_stage(1, 0);
  EXPECT_NEAR(s.x, 10, 1e-12);
  EXPECT_NEAR(s.y, 21, 1e-12);
}

struct BadSet {
  const char* name;
  std::vector<CalibrationPoint> points;
  const char* says;
  friend void PrintTo(const BadSet& b, std::ostream* os) { *os << b.name; }
};

class SolveBad : public ::testing::TestWithParam<BadSet> {};

TEST_P(SolveBad, RefusesDegenerateSets) {
  const auto map = small();
  const auto s = solve(map, GetParam().points);
  ASSERT_FALSE(s);
  EXPECT_EQ(s.error().kind, ErrorKind::Config);
  EXPECT_NE(s.error().what.find(GetParam().says), std::string::npos) << s.error().what;
}

INSTANTIATE_TEST_SUITE_P(
    Sets, SolveBad,
    ::testing::Values(BadSet{"unknown hole", {{"99", 1, 2}}, "99"},
                      BadSet{"same hole twice", {{"1", 1, 2}, {"1", 3, 4}}, "twice"},
                      BadSet{"one stage position", {{"1", 1, 2}, {"3", 1, 2}}, "same stage position"},
                      BadSet{"one stage position of three", {{"1", 0, 0}, {"3", 5, 0}, {"2", 5, 0}}, "same stage position"},
                      BadSet{"scale", {{"1", 0, 0}, {"3", 5.25, 0}, {"2", 0, 5.25}}, "scale"},
                      BadSet{"infinite", {{"1", std::numeric_limits<double>::infinity(), 0}}, "finite"},
                      BadSet{"nan", {{"1", 0, 0}, {"3", 5, std::numeric_limits<double>::quiet_NaN()}}, "finite"}));

// Two holes exchanged by mistake must not give a calibration that looks good.
TEST(Solve, SwappedHolesAmongManyAreRefused) {
  const auto map = small();
  const Transform truth{12, 8, 2 * kDeg, 1};
  std::vector<CalibrationPoint> points{at(map, truth, "1"), at(map, truth, "2"), at(map, truth, "3"),
                                       at(map, truth, "4"), at(map, truth, "5")};
  std::swap(points[1].hole, points[3].hole);
  const auto s = solve(map, points);
  ASSERT_FALSE(s);
  EXPECT_NE(s.error().what.find("scale"), std::string::npos) << s.error().what;
}

// Points on one line fix rotation and scale along it; they are still a
// calibration, and a miss still shows.
TEST(Solve, CollinearPointsSolveAndAMissShows) {
  const auto map = small();
  const Transform truth{12, 8, 5 * kDeg, 1};
  std::vector<CalibrationPoint> points{at(map, truth, "5"), at(map, truth, "1"), at(map, truth, "3", 0, 0.6)};
  const auto s = solve(map, points);
  ASSERT_TRUE(s) << s.error().what;
  EXPECT_GT(s->rms_mm, 0.1);
  const auto far = s->transform.to_stage(0, 5);  // hole 2, off the line
  const auto want = truth.to_stage(0, 5);
  EXPECT_LT(std::hypot(far.x - want.x, far.y - want.y), 1.0);
}

// What the points cannot rule out. Two points exchanged fit perfectly with
// the tray turned half way round, and points on one line fit as well with
// the other axis mirrored: neither shows in the rms, so both are said.
TEST(Cautions, TwoExchangedPointsLookLikeAHalfTurn) {
  const auto map = small();
  const std::vector<CalibrationPoint> swapped{{"1", 30, 25}, {"3", 25, 25}};  // truly 1 at 25, 3 at 30
  const auto s = solve(map, swapped);
  ASSERT_TRUE(s);
  EXPECT_NEAR(s->rms_mm, 0, 1e-9);
  const auto said = cautions(map, swapped, *s);
  ASSERT_FALSE(said.empty());
  bool turned = false;
  for (const auto& c : said) turned = turned || c.find("180") != std::string::npos;
  EXPECT_TRUE(turned) << ::testing::PrintToString(said);
}

TEST(Cautions, PointsOnOneLineCannotShowAMirroredAxis) {
  const auto map = small();
  const Transform truth{12, 8, 0, 1};
  const std::vector<CalibrationPoint> two{at(map, truth, "1"), at(map, truth, "3")};
  const std::vector<CalibrationPoint> line{at(map, truth, "5"), at(map, truth, "1"), at(map, truth, "3")};
  const std::vector<CalibrationPoint> spread{at(map, truth, "1"), at(map, truth, "3"), at(map, truth, "2")};
  const std::vector<CalibrationPoint> one{at(map, truth, "1")};
  const auto mirror = [&](const std::vector<CalibrationPoint>& points) {
    const auto s = solve(map, points);
    EXPECT_TRUE(s);
    for (const auto& c : cautions(map, points, *s))
      if (c.find("mirror") != std::string::npos) return true;
    return false;
  };
  EXPECT_TRUE(mirror(two));
  EXPECT_TRUE(mirror(line));
  EXPECT_FALSE(mirror(spread));
  EXPECT_TRUE(mirror(one));
  const auto s = solve(map, spread);
  EXPECT_TRUE(cautions(map, spread, *s).empty());  // a good, spread fit says nothing
}

TEST(Cautions, AQuarterTurnIsSaid) {
  const auto map = small();
  const Transform truth{12, 8, 90 * kDeg, 1};
  const std::vector<CalibrationPoint> points{at(map, truth, "1"), at(map, truth, "3"), at(map, truth, "2")};
  const auto s = solve(map, points);
  ASSERT_TRUE(s);
  const auto said = cautions(map, points, *s);
  ASSERT_EQ(said.size(), 1u);
  EXPECT_NE(said[0].find("90"), std::string::npos) << said[0];
}

TEST(Solve, ErrorTextDoesNotDependOnTheLocale) {
  const auto map = small();
  const std::vector<CalibrationPoint> points{{"1", 0, 0}, {"3", 5.25, 0}, {"2", 0, 5.25}};
  const auto s = solve(map, points);
  ASSERT_FALSE(s);
  EXPECT_NE(s.error().what.find("1.050"), std::string::npos) << s.error().what;
}
