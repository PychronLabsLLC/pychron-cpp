#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "pychron/vision/source.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron::vision;

TEST(Synth, HoleAtCenterWhenStageOnHole) {
  HoleScene s;
  s.hole_mm = {1.0, -2.0};
  auto [f, truth] = render(s, {1.0, -2.0});
  EXPECT_NEAR(truth.center_px.x, (s.width - 1) / 2.0, 1e-9);
  EXPECT_NEAR(truth.center_px.y, (s.height - 1) / 2.0, 1e-9);
  EXPECT_NEAR(truth.radius_px, 0.5 * 23.0, 1e-9);
  EXPECT_TRUE(truth.visible);
  EXPECT_LT(f.view().at(100, 100), 0.4 * s.pixel_depth);
  EXPECT_GT(f.view().at(0, 0), 0.6 * s.pixel_depth);
}

TEST(Synth, StageMoveShiftsSceneOpposite) {
  HoleScene s;
  const auto a = render(s, {0, 0}).second;
  const auto b = render(s, {0.1, 0}).second;
  EXPECT_NEAR(b.center_px.x - a.center_px.x, -2.3, 1e-9);
  EXPECT_NEAR(b.center_px.y, a.center_px.y, 1e-9);
}

TEST(Synth, StageMovePositiveYShiftsImageDown) {
  HoleScene s;
  const auto a = render(s, {0, 0}).second;
  const auto b = render(s, {0, 0.1}).second;
  EXPECT_NEAR(b.center_px.y - a.center_px.y, +2.3, 1e-9);
  EXPECT_NEAR(b.center_px.x, a.center_px.x, 1e-9);
}

TEST(Synth, GlowPeakAboveOneSaturates) {
  GlowScene s;
  s.peak = 2.0;
  auto [f, truth] = render(s, {0, 0});
  const auto m = *std::max_element(f.data.begin(), f.data.end());
  EXPECT_EQ(m, s.pixel_depth);
  EXPECT_NEAR(truth.radius_px, 0.3 * 23.0, 1e-9);
}

TEST(Synth, SameSeedSameFrame) {
  HoleScene s;
  s.noise = 0.05;
  s.seed = 42;
  const auto a = render(s, {0.013, -0.007}).first;
  const auto b = render(s, {0.013, -0.007}).first;
  EXPECT_EQ(a.data, b.data);
  s.seed = 43;
  EXPECT_NE(a.data, render(s, {0.013, -0.007}).first.data);
}

TEST(Synth, SixteenBitScalesLevels) {
  HoleScene s;
  s.pixel_depth = 4095;
  auto [f, truth] = render(s, {0, 0});
  EXPECT_NEAR(f.view().at(100, 100), 0.25 * 4095, 2);
  EXPECT_NEAR(f.view().at(0, 0), 0.75 * 4095, 2);
}

TEST(Synth, SubPixelCenterIsHonoured) {
  // The intensity-weighted centroid of the hole follows the non-integer truth.
  HoleScene s;
  const Vec2 stage{0.0217, -0.0131};
  auto [f, truth] = render(s, stage);
  double sw = 0, sx = 0, sy = 0;
  for (int y = 0; y < f.height; ++y)
    for (int x = 0; x < f.width; ++x) {
      // Tray pixels quantise a hair below the tray level; skip them so only
      // the hole (and its edge ramp) contributes.
      const double v = f.view().at(x, y);
      if (v >= 0.7 * s.pixel_depth) continue;
      const double w = 0.75 * s.pixel_depth - v;
      sw += w;
      sx += w * x;
      sy += w * y;
    }
  EXPECT_NEAR(sx / sw, truth.center_px.x, 0.05);
  EXPECT_NEAR(sy / sw, truth.center_px.y, 0.05);
}

TEST(Synth, NeighboursSitOnPitchGrid) {
  HoleScene s;
  s.neighbours = true;
  auto [f, truth] = render(s, {0, 0});
  const double pitch = s.pitch_mm * s.px_per_mm;
  const int nx = static_cast<int>(std::lround(truth.center_px.x + pitch));
  const int ny = static_cast<int>(std::lround(truth.center_px.y));
  EXPECT_LT(f.view().at(nx, ny), 0.4 * s.pixel_depth);
  const int mx = static_cast<int>(std::lround(truth.center_px.x + pitch / 2));
  EXPECT_GT(f.view().at(mx, ny), 0.6 * s.pixel_depth);
}

TEST(Synth, TargetOffFrameIsNotVisible) {
  HoleScene s;
  s.hole_mm = {20.0, 0.0};
  EXPECT_FALSE(render(s, {0, 0}).second.visible);
}

TEST(Synth, CrosshairDrawnThroughCenter) {
  HoleScene h;
  h.width = h.height = 201;
  h.crosshair = true;
  auto fh = render(h, {5.0, 5.0}).first;  // hole far off frame: tray everywhere else
  EXPECT_EQ(fh.view().at(100, 3), 0);
  EXPECT_GT(fh.view().at(3, 3), 0.6 * h.pixel_depth);
  GlowScene g;
  g.width = g.height = 201;
  g.crosshair = true;
  auto fg = render(g, {5.0, 5.0}).first;
  EXPECT_NEAR(fg.view().at(3, 100), 0.6 * g.pixel_depth, 1);
}

TEST(SyntheticSource, GrabStampsFramesFromClockAndIncrementsSeq) {
  pychron::TimePoint now{};
  Vec2 stage{0, 0};
  HoleScene s;
  SyntheticSource src(s, [&] { return stage; }, [&] { return now; });

  now += std::chrono::milliseconds(5);
  auto a = src.grab();
  ASSERT_TRUE(a.has_value());
  const Truth ta = src.last_truth();
  now += std::chrono::milliseconds(33);
  stage = {0.1, 0};
  auto b = src.grab();
  ASSERT_TRUE(b.has_value());

  EXPECT_EQ(a->timestamp, pychron::TimePoint{} + std::chrono::milliseconds(5));
  EXPECT_EQ(b->timestamp, pychron::TimePoint{} + std::chrono::milliseconds(38));
  EXPECT_EQ(b->seq, a->seq + 1);
  EXPECT_NEAR(src.last_truth().center_px.x - ta.center_px.x, -2.3, 1e-9);
  EXPECT_EQ(src.info().width, 200);
  EXPECT_EQ(src.info().pixel_depth, 255);
}

TEST(Synth, CrosshairIsOnePixelWideOnEvenAndOddFrames) {
  for (int n : {200, 201}) {
    HoleScene h;
    h.width = h.height = n;
    h.crosshair = true;
    auto f = render(h, {5.0, 5.0}).first;  // hole off frame, tray elsewhere
    int dark_cols = 0, dark_rows = 0;
    for (int x = 0; x < n; ++x) dark_cols += f.view().at(x, 3) == 0 ? 1 : 0;
    for (int y = 0; y < n; ++y) dark_rows += f.view().at(3, y) == 0 ? 1 : 0;
    EXPECT_EQ(dark_cols, 1) << n;
    EXPECT_EQ(dark_rows, 1) << n;
    EXPECT_EQ(f.view().at((n - 1) / 2, 3), 0);
    EXPECT_EQ(f.view().at(3, (n - 1) / 2), 0);
  }
}

TEST(Synth, ZeroSigmaAndElongationRenderWithoutNaN) {
  GlowScene s;
  s.sigma_mm = 0.0;
  s.elongation = 0.0;
  auto [f, truth] = render(s, {0, 0});
  EXPECT_FALSE(f.data.empty());
  for (auto v : f.data) EXPECT_LE(v, s.pixel_depth);
  EXPECT_TRUE(std::isfinite(truth.radius_px));
}

TEST(SyntheticSource, SuccessiveGrabsHaveIndependentNoiseButSourcesReplay) {
  HoleScene s;
  s.noise = 0.05;
  auto make = [&] {
    return SyntheticSource(s, [] { return Vec2{}; }, [] { return pychron::TimePoint{}; });
  };
  auto a = make();
  auto b = make();
  auto a1 = a.grab();
  auto a2 = a.grab();
  auto b1 = b.grab();
  auto b2 = b.grab();
  ASSERT_TRUE(a1.has_value() && a2.has_value() && b1.has_value() && b2.has_value());
  EXPECT_NE(a1->data, a2->data);
  EXPECT_EQ(a1->data, b1->data);
  EXPECT_EQ(a2->data, b2->data);
}

TEST(Synth, NonPositiveSizeRendersEmptyFrame) {
  for (auto [w, h] : {std::pair{0, 200}, std::pair{200, -5}}) {
    HoleScene hs;
    hs.width = w;
    hs.height = h;
    auto fh = render(hs, {0, 0}).first;
    EXPECT_TRUE(fh.data.empty());
    EXPECT_TRUE(fh.width == 0 || fh.height == 0);
    GlowScene gs;
    gs.width = w;
    gs.height = h;
    auto fg = render(gs, {0, 0}).first;
    EXPECT_TRUE(fg.data.empty());
    EXPECT_TRUE(fg.width == 0 || fg.height == 0);
  }
}

TEST(Synth, NanStagePositionRendersAFrameWithoutUB) {
  const double nan = std::nan("");
  HoleScene h;
  auto [hf, ht] = render(h, {nan, 0.0});
  EXPECT_EQ(hf.width, h.width);
  EXPECT_EQ(hf.data.size(), static_cast<std::size_t>(h.width) * static_cast<std::size_t>(h.height));
  EXPECT_FALSE(ht.visible);
  GlowScene g;
  auto [gf, gt] = render(g, {0.0, nan});
  EXPECT_EQ(gf.width, g.width);
  EXPECT_FALSE(gt.visible);
  GlowScene bad = g;
  bad.peak = nan;
  bad.noise = nan;
  auto [bf, bt] = render(bad, {0.0, 0.0});
  EXPECT_EQ(bf.data.size(), gf.data.size());
}

TEST(Synth, NanGlowParametersDoNotReachTheCast) {
  GlowScene g;
  g.background = std::nan("");
  auto [f, t] = render(g, {0.0, 0.0});
  (void)t;
  EXPECT_FALSE(f.data.empty());
}
