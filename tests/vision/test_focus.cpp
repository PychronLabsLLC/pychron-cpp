#include <gtest/gtest.h>

#include <cmath>

#include "pychron/vision/focus.hpp"
#include "pychron/vision/kernel.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron::vision;

namespace {
HoleScene scene(std::uint16_t depth = 255) {
  HoleScene s;
  s.neighbours = true;
  s.noise = 0;
  s.pixel_depth = depth;
  return s;
}

struct Scores {
  double lap, var, sob;
};

Scores score(const FrameView& v) { return {focus::laplace_p99(v), focus::variance(v), focus::sobel_sum(v)}; }
}  // namespace

TEST(Focus, SharpSceneScoresHigherThanBlurred) {
  const auto [f, truth] = render(scene(), {});
  const auto blurred = box_blur(f.view(), 3);
  const auto sharp = score(f.view());
  const auto soft = score(blurred.view());
  EXPECT_GT(sharp.lap, soft.lap);
  EXPECT_GT(sharp.var, soft.var);
  EXPECT_GT(sharp.sob, soft.sob);
  EXPECT_GT(sharp.lap, 0);
  EXPECT_GT(sharp.var, 0);
  EXPECT_GT(sharp.sob, 0);
}

TEST(Focus, MonotonicWithBlurRadius) {
  const auto [f, truth] = render(scene(), {});
  double prev = focus::laplace_p99(f.view());  // radius 0
  for (int r : {1, 2, 4}) {
    const auto b = box_blur(f.view(), r);
    const double s = focus::laplace_p99(b.view());
    EXPECT_LT(s, prev) << "radius " << r;
    prev = s;
  }
}

TEST(Focus, ConstantFrameScoresZero) {
  const auto f = Frame::make(32, 32, 255, 100);
  const auto s = score(f.view());
  EXPECT_EQ(s.lap, 0);
  EXPECT_EQ(s.var, 0);
  EXPECT_EQ(s.sob, 0);
}

TEST(Focus, TinyFrameScoresZero) {
  auto two = Frame::make(2, 2, 255, 0);
  two.at(0, 0) = 255;
  two.at(1, 1) = 255;
  const auto a = score(two.view());
  EXPECT_EQ(a.lap, 0);
  EXPECT_EQ(a.var, 0);
  EXPECT_EQ(a.sob, 0);

  const auto empty = Frame::make(0, 0, 255);
  const auto b = score(empty.view());
  EXPECT_EQ(b.lap, 0);
  EXPECT_EQ(b.var, 0);
  EXPECT_EQ(b.sob, 0);

  auto thin = Frame::make(10, 2, 255, 0);
  thin.at(3, 0) = 255;
  const auto c = score(thin.view());
  EXPECT_EQ(c.lap, 0);
  EXPECT_EQ(c.var, 0);
  EXPECT_EQ(c.sob, 0);
}

TEST(Focus, DegenerateViewsScoreZero) {
  auto f = Frame::make(8, 8, 255, 0);
  f.at(4, 4) = 255;
  FrameView nulled = f.view();
  nulled.data = nullptr;
  const auto a = score(nulled);
  EXPECT_EQ(a.lap, 0);
  EXPECT_EQ(a.var, 0);
  EXPECT_EQ(a.sob, 0);

  FrameView nodepth = f.view();
  nodepth.pixel_depth = 0;
  const auto b = score(nodepth);
  EXPECT_EQ(b.lap, 0);
  EXPECT_EQ(b.var, 0);
  EXPECT_EQ(b.sob, 0);
}

TEST(Focus, DepthIndependent) {
  const auto lo = render(scene(255), {}).first;
  const auto hi = render(scene(4095), {}).first;
  const auto a = score(lo.view());
  const auto b = score(hi.view());
  EXPECT_NEAR(a.lap, b.lap, 0.05 * a.lap);
  EXPECT_NEAR(a.var, b.var, 0.05 * a.var);
  EXPECT_NEAR(a.sob, b.sob, 0.05 * a.sob);
}
