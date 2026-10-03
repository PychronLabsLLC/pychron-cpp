#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "pychron/vision/calibration.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron::vision;
using pychron::ErrorKind;

namespace {

// Image deltas that a known centring map M produces for the given stage jogs:
// a = M^-1 * (-s).
std::vector<JogPair> pairs_from(const double M[2][2], const std::vector<Vec2>& stage_jogs) {
  const double det = M[0][0] * M[1][1] - M[0][1] * M[1][0];
  std::vector<JogPair> out;
  for (const Vec2 s : stage_jogs) {
    const double tx = -s.x, ty = -s.y;
    out.push_back({s, {(M[1][1] * tx - M[0][1] * ty) / det, (-M[1][0] * tx + M[0][0] * ty) / det}});
  }
  return out;
}

Vec2 offset_of(const HoleScene& scene, Vec2 stage) {
  auto [frame, truth] = render(scene, stage);
  FinderParams p;
  p.mode = FinderMode::Hole;
  p.expected_radius_px = scene.hole_radius_mm * scene.px_per_mm;
  const auto found = SimpleFinder{}.find(frame.view(), p);
  EXPECT_FALSE(found.empty());
  if (found.empty()) return {};
  return {found.front().center_px.x, found.front().center_px.y};
}

}  // namespace

TEST(CameraStageMap, FromScaleMatchesSynthConvention) {
  const auto map = CameraStageMap::from_scale(23.0, false, true);
  const Vec2 a = map.to_mm({23, 0});
  const Vec2 b = map.to_mm({0, 23});
  EXPECT_EQ(a.x, 1.0);
  EXPECT_EQ(a.y, 0.0);
  EXPECT_EQ(b.x, 0.0);
  EXPECT_EQ(b.y, -1.0);
  EXPECT_TRUE(map.valid());
}

TEST(CameraStageMap, FromScaleFlipsNegateAxes) {
  const auto map = CameraStageMap::from_scale(10.0, true, false);
  EXPECT_EQ(map.to_mm({10, 0}).x, -1.0);
  EXPECT_EQ(map.to_mm({0, 10}).y, 1.0);
}

TEST(CameraStageMap, SolveRecoversRotationFlipAndScale) {
  const double th = 17.0 * std::numbers::pi / 180.0;
  const double c = std::cos(th) / 31.0, s = std::sin(th) / 31.0;
  // R(17deg) * diag(-1, 1) / 31
  const double M[2][2] = {{-c, -s}, {-s, c}};
  const auto pairs = pairs_from(M, {{0.2, 0}, {0, 0.2}, {-0.15, 0.1}});
  const auto r = CameraStageMap::solve(pairs);
  ASSERT_TRUE(r.has_value());
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) EXPECT_LT(std::abs(r->m[i][j] - M[i][j]), 1e-9);
  EXPECT_LT(r->residual_mm, 1e-9);
}

TEST(CameraStageMap, SolveAgreesWithSyntheticScene) {
  HoleScene scene;
  scene.hole_mm = {0.3, -0.2};
  const Vec2 c0 = offset_of(scene, {0, 0});
  const Vec2 cx = offset_of(scene, {0.2, 0});
  const Vec2 cy = offset_of(scene, {0, 0.2});
  const std::vector<JogPair> pairs = {{{0.2, 0}, {cx.x - c0.x, cx.y - c0.y}},
                                      {{0, 0.2}, {cy.x - c0.x, cy.y - c0.y}}};
  const auto r = CameraStageMap::solve(pairs);
  ASSERT_TRUE(r.has_value());

  const Vec2 stage{0.1, -0.15};
  const Vec2 found = offset_of(scene, stage);
  const Vec2 centre{(scene.width - 1) / 2.0, (scene.height - 1) / 2.0};
  const Vec2 move = r->to_mm({found.x - centre.x, found.y - centre.y});
  // The stage must move by (hole - stage) to centre the hole.
  EXPECT_NEAR(move.x, scene.hole_mm.x - stage.x, 0.01);
  EXPECT_NEAR(move.y, scene.hole_mm.y - stage.y, 0.01);
}

TEST(CameraStageMap, SolveRejectsCollinearPairs) {
  const std::vector<JogPair> pairs = {{{0.1, 0.1}, {5, 5}}, {{0.2, 0.2}, {10, 10}}, {{-0.1, -0.1}, {-5, -5}}};
  const auto r = CameraStageMap::solve(pairs);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(CameraStageMap, SolveRejectsZeroLengthSecondDelta) {
  // A zero image delta adds nothing, so one real direction cannot span the plane.
  const std::vector<JogPair> pairs = {{{0.1, 0}, {5, 0}}, {{0.0, 0.2}, {0, 0}}};
  const auto r = CameraStageMap::solve(pairs);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(CameraStageMap, SolveRejectsFewerThanTwoPairs) {
  const std::vector<JogPair> one = {{{0.1, 0}, {5, 0}}};
  const auto r1 = CameraStageMap::solve(one);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error().kind, ErrorKind::Config);
  const auto r0 = CameraStageMap::solve({});
  ASSERT_FALSE(r0.has_value());
  EXPECT_EQ(r0.error().kind, ErrorKind::Config);
}

TEST(CameraStageMap, SolveRejectsNonFiniteInput) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<JogPair> pairs = {{{0.1, 0}, {5, 0}}, {{0, 0.1}, {nan, 5}}};
  const auto r = CameraStageMap::solve(pairs);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}

TEST(CameraStageMap, ResidualReportsNoisyFit) {
  const double M[2][2] = {{0.03, 0}, {0, -0.03}};
  auto pairs = pairs_from(M, {{0.2, 0}, {0, 0.2}, {-0.2, 0}, {0, -0.2}});
  pairs[0].image_delta_px.x += 1.0;
  pairs[1].image_delta_px.y -= 1.0;
  const auto r = CameraStageMap::solve(pairs);
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->residual_mm, 0.0);
}

TEST(CameraStageMap, SingularMapIsInvalid) {
  CameraStageMap zero;
  zero.m[0][0] = zero.m[1][1] = 0;
  EXPECT_FALSE(zero.valid());

  CameraStageMap nan_map;
  nan_map.m[0][1] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(nan_map.valid());

  EXPECT_TRUE(CameraStageMap{}.valid());
}

TEST(CameraStageMap, TomlRoundTrip) {
  const double th = 17.0 * std::numbers::pi / 180.0;
  CameraStageMap map;
  map.m[0][0] = -std::cos(th) / 31.0;
  map.m[0][1] = -std::sin(th) / 31.0;
  map.m[1][0] = 1e-7 / 3.0;
  map.m[1][1] = 1.0;
  map.residual_mm = 0.0123456789012345;
  const auto r = CameraStageMap::from_toml(map.to_toml());
  ASSERT_TRUE(r.has_value());
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) EXPECT_EQ(r->m[i][j], map.m[i][j]);
  EXPECT_EQ(r->residual_mm, map.residual_mm);
}

TEST(CameraStageMap, FromTomlResidualOptionalAndIntegersAccepted) {
  const auto r = CameraStageMap::from_toml("m = [[2, 0], [0, 3]]");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->m[0][0], 2.0);
  EXPECT_EQ(r->residual_mm, 0.0);
}

TEST(CameraStageMap, FromTomlRejectsWrongShape) {
  for (const char* text : {"m = [[1, 0, 0], [0, 1, 0]]", "m = [[1, 0]]", "m = [1, 0, 0, 1]", "residual_mm = 1.0",
                           "m = [[1, 0], [0, \"x\"]]", "m = [[0, 0], [0, 0]]", "m = [[nan, 0], [0, 1]]",
                           "m = [[1, 0], [0, 1]]\nresidual_mm = \"x\"", "m = ["}) {
    const auto r = CameraStageMap::from_toml(text);
    ASSERT_FALSE(r.has_value()) << text;
    EXPECT_EQ(r.error().kind, ErrorKind::Config) << text;
  }
}
