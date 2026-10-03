#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "pychron/vision/finder.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron::vision;

namespace {

double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

FinderParams hole_params(const HoleScene& s) {
  FinderParams p;
  p.mode = FinderMode::Hole;
  p.expected_radius_px = s.hole_radius_mm * s.px_per_mm;
  return p;
}

FinderParams glow_params() {
  FinderParams p;
  p.mode = FinderMode::Glow;
  return p;
}

}  // namespace

TEST(SimpleFinder, CleanHoleWithinHalfPixel) {
  HoleScene s;
  auto [f, truth] = render(s, {0.13, -0.21});
  SimpleFinder finder;
  const auto r = finder.find(f.view(), hole_params(s));
  ASSERT_EQ(r.size(), 1u);
  EXPECT_LT(dist(r[0].center_px, truth.center_px), 0.5);
}

TEST(SimpleFinder, NoisyHoleWithOverlaysAndNeighboursWithin1p5) {
  HoleScene s;
  s.noise = 0.03;
  s.neighbours = s.glint = s.crosshair = s.shadow = true;
  auto [f, truth] = render(s, {0.13, -0.21});
  SimpleFinder finder;
  const auto r = finder.find(f.view(), hole_params(s));
  ASSERT_GE(r.size(), 1u);
  EXPECT_LT(dist(r[0].center_px, truth.center_px), 1.5);
}

TEST(SimpleFinder, PicksHoleNearestCentreAmongNeighbours) {
  HoleScene s;
  s.neighbours = true;
  auto [f, truth] = render(s, {-0.2, 0.1});
  SimpleFinder finder;
  const auto r = finder.find(f.view(), hole_params(s));
  ASSERT_GE(r.size(), 2u);  // neighbours are complete holes inside the frame
  EXPECT_LT(dist(r[0].center_px, truth.center_px), 0.5);
  const double c = (f.width - 1) / 2.0;
  for (std::size_t i = 1; i < r.size(); ++i)
    EXPECT_LE(std::hypot(r[i - 1].center_px.x - c, r[i - 1].center_px.y - c),
              std::hypot(r[i].center_px.x - c, r[i].center_px.y - c));
}

TEST(SimpleFinder, RejectsHoleCutByMaskEdge) {
  HoleScene s;
  auto [f, truth] = render(s, {-12.0 / s.px_per_mm, 0});  // hole 12 px right of centre
  FinderParams p = hole_params(s);
  p.mask_radius_px = 15;
  FinderDebug dbg;
  SimpleFinder finder;
  EXPECT_TRUE(finder.find(f.view(), p, &dbg).empty());
  EXPECT_GE(dbg.rejected_edge, 1);
}

TEST(SimpleFinder, RejectsWrongRadius) {
  HoleScene s;
  auto [f, truth] = render(s, {0, 0});
  FinderParams p = hole_params(s);
  p.expected_radius_px *= 2;
  FinderDebug dbg;
  SimpleFinder finder;
  EXPECT_TRUE(finder.find(f.view(), p, &dbg).empty());
  EXPECT_GE(dbg.rejected_radius, 1);
}

TEST(SimpleFinder, CleanGlowWithinHalfPixel) {
  GlowScene s;
  auto [f, truth] = render(s, {0.13, -0.21});
  SimpleFinder finder;
  const auto r = finder.find(f.view(), glow_params());
  ASSERT_EQ(r.size(), 1u);
  EXPECT_LT(dist(r[0].center_px, truth.center_px), 0.5);
}

TEST(SimpleFinder, ElongatedSaturatedGlowCentroidWithin1p5) {
  GlowScene s;
  s.elongation = 2.0;
  s.peak = 2.0;
  auto [f, truth] = render(s, {0.13, -0.21});
  SimpleFinder finder;
  const auto r = finder.find(f.view(), glow_params());
  ASSERT_EQ(r.size(), 1u);
  EXPECT_LT(dist(r[0].center_px, truth.center_px), 1.5);
}

TEST(SimpleFinder, GlowSaturationRisesWithPeak) {
  SimpleFinder finder;
  double prev = -1;
  for (const double peak : {0.3, 0.9, 2.0}) {
    GlowScene s;
    s.peak = peak;
    const auto r = finder.find(render(s, {0, 0}).first.view(), glow_params());
    ASSERT_EQ(r.size(), 1u) << "peak " << peak;
    const double sat = saturation(r[0]);
    EXPECT_GE(sat, 0.0);
    EXPECT_LE(sat, 1.0);
    EXPECT_GT(sat, prev) << "peak " << peak;
    prev = sat;
  }
}

TEST(SimpleFinder, SixteenBitMatchesEightBit) {
  SimpleFinder finder;
  GlowScene g8, g16;
  g16.pixel_depth = 65535;
  const auto a = finder.find(render(g8, {0.13, -0.21}).first.view(), glow_params());
  const auto b = finder.find(render(g16, {0.13, -0.21}).first.view(), glow_params());
  ASSERT_EQ(a.size(), 1u);
  ASSERT_EQ(b.size(), 1u);
  EXPECT_LT(dist(a[0].center_px, b[0].center_px), 0.2);
  EXPECT_NEAR(saturation(a[0]), saturation(b[0]), 0.02);

  HoleScene h8, h16;
  h16.pixel_depth = 65535;
  const auto c = finder.find(render(h8, {0.13, -0.21}).first.view(), hole_params(h8));
  const auto d = finder.find(render(h16, {0.13, -0.21}).first.view(), hole_params(h16));
  ASSERT_EQ(c.size(), 1u);
  ASSERT_EQ(d.size(), 1u);
  EXPECT_LT(dist(c[0].center_px, d[0].center_px), 0.2);
}

TEST(SimpleFinder, UniformFramesGiveNoTarget) {
  SimpleFinder finder;
  for (const std::uint16_t fill : {0, 255, 128}) {
    const Frame f = Frame::make(120, 120, 255, fill);
    FinderParams hp;
    hp.mode = FinderMode::Hole;
    hp.expected_radius_px = 11.5;
    EXPECT_TRUE(finder.find(f.view(), hp).empty()) << "hole, fill " << fill;
    EXPECT_TRUE(finder.find(f.view(), glow_params()).empty()) << "glow, fill " << fill;
  }
}

TEST(SimpleFinder, GlowBelowMinimumAreaIgnored) {
  // A square blob survives the median and the contrast gate. With a high
  // glow_fraction only its core passes the threshold: 5 px for a 5x5 blob
  // (below the 9 px minimum), 12 px for a 6x6 blob.
  auto blob = [](int side) {
    Frame f = Frame::make(100, 100, 255, 5);
    for (int y = 40; y < 40 + side; ++y)
      for (int x = 40; x < 40 + side; ++x) f.at(x, y) = 255;
    return f;
  };
  FinderParams p = glow_params();
  p.glow_fraction = 0.9;
  SimpleFinder finder;

  FinderDebug small;
  EXPECT_TRUE(finder.find(blob(5).view(), p, &small).empty());
  EXPECT_GE(small.components, 1);
  EXPECT_GE(small.rejected_area, 1);
  EXPECT_LT(std::count(small.mask.begin(), small.mask.end(), 1), 9);

  FinderDebug large;
  EXPECT_EQ(finder.find(blob(6).view(), p, &large).size(), 1u);
  EXPECT_EQ(large.rejected_area, 0);
  EXPECT_GE(std::count(large.mask.begin(), large.mask.end(), 1), 9);
}

TEST(SimpleFinder, GlowFractionOutOfRangeIsClamped) {
  GlowScene s;
  auto [f, truth] = render(s, {0, 0});
  SimpleFinder finder;
  FinderParams p = glow_params();
  p.glow_fraction = -1;  // clamps to 0: threshold at the floor, no wrap
  FinderDebug low;
  finder.find(f.view(), p, &low);  // must not crash
  EXPECT_LE(low.threshold, f.pixel_depth);
  p.glow_fraction = 5;  // clamps to 1: threshold at the maximum, nothing above it but the peak
  FinderDebug high;
  EXPECT_TRUE(finder.find(f.view(), p, &high).empty());
  EXPECT_LE(high.threshold, f.pixel_depth);
  EXPECT_GT(high.threshold, low.threshold);
}

TEST(SimpleFinder, MaskLargerThanFrameDoesNotReadOutOfBounds) {
  HoleScene hs;
  GlowScene gs;
  auto [hf, ht] = render(hs, {0, 0});
  auto [gf, gt] = render(gs, {0, 0});
  FinderParams hp = hole_params(hs);
  hp.mask_radius_px = 10.0 * hf.width;
  FinderParams gp = glow_params();
  gp.mask_radius_px = 10.0 * gf.width;
  SimpleFinder finder;
  const auto h = finder.find(hf.view(), hp);
  const auto g = finder.find(gf.view(), gp);
  ASSERT_EQ(h.size(), 1u);
  ASSERT_EQ(g.size(), 1u);
  EXPECT_LT(dist(h[0].center_px, ht.center_px), 0.5);
  EXPECT_LT(dist(g[0].center_px, gt.center_px), 0.5);
}

TEST(SimpleFinder, ZeroSizeFrameGivesNoTarget) {
  const Frame f = Frame::make(0, 0, 255);
  SimpleFinder finder;
  FinderDebug dbg;
  EXPECT_TRUE(finder.find(f.view(), glow_params(), &dbg).empty());
  FinderParams hp;
  hp.mode = FinderMode::Hole;
  EXPECT_TRUE(finder.find(f.view(), hp, &dbg).empty());
  EXPECT_TRUE(dbg.mask.empty());
}

TEST(SimpleFinder, DebugIsFilledWhenRequested) {
  HoleScene s;
  auto [f, truth] = render(s, {0, 0});
  FinderDebug dbg;
  SimpleFinder finder;
  ASSERT_EQ(finder.find(f.view(), hole_params(s), &dbg).size(), 1u);
  EXPECT_EQ(dbg.width, f.width);
  EXPECT_EQ(dbg.height, f.height);
  ASSERT_EQ(dbg.mask.size(), f.data.size());
  EXPECT_GT(dbg.threshold, 0);
  EXPECT_GE(dbg.components, 1);
  EXPECT_GT(std::count(dbg.mask.begin(), dbg.mask.end(), 1), 0);
  EXPECT_EQ(dbg.mask[static_cast<std::size_t>(100 * f.width + 100)], 1);  // inside the hole
  EXPECT_EQ(dbg.mask[0], 0);                                              // tray corner
}

// Non-finite or absurd parameters find nothing and never reach lround / int arithmetic.
TEST(SimpleFinder, NonFiniteOrHugeParametersFindNothing) {
  HoleScene hs;
  const auto hole = render(hs, {0.0, 0.0}).first;
  GlowScene gs;
  gs.peak = 0.6;
  const auto glow = render(gs, {0.0, 0.0}).first;
  const double nan = std::nan(""), inf = std::numeric_limits<double>::infinity();
  SimpleFinder finder;
  for (const double bad : {nan, inf, -inf, 1e300}) {
    for (int field = 0; field < 4; ++field) {
      for (int m = 0; m < 2; ++m) {
        FinderParams p = m == 0 ? hole_params(hs) : glow_params();
        if (m == 0) p.expected_radius_px = hs.hole_radius_mm * hs.px_per_mm;
        double& slot = field == 0 ? p.expected_radius_px : field == 1 ? p.radius_tol : field == 2 ? p.mask_radius_px : p.glow_fraction;
        slot = bad;
        const auto r = finder.find((m == 0 ? hole : glow).view(), p);
        if (!std::isfinite(bad)) {
          EXPECT_TRUE(r.empty()) << "field " << field << " mode " << m;
        }
        // 1e300 is finite: it only has to be well defined (no UB under the sanitizers).
      }
    }
  }
}

TEST(SimpleFinder, ZeroPixelDepthFindsNothing) {
  Frame f = Frame::make(40, 40, 0);
  SimpleFinder finder;
  FinderParams p;
  p.expected_radius_px = 10;
  EXPECT_TRUE(finder.find(f.view(), p).empty());
}
