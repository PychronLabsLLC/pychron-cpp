#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "pychron/vision/kernel.hpp"

using namespace pychron::vision;

namespace {
std::vector<std::uint8_t> disk_mask(int w, int h, double cx, double cy, double r_out, double r_in) {
  std::vector<std::uint8_t> m(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double d = std::hypot(x - cx, y - cy);
      if (d <= r_out && d >= r_in) m[static_cast<std::size_t>(y * w + x)] = 1;
    }
  return m;
}
}  // namespace

TEST(Kernel, Median3RemovesOnePixelLine) {
  auto f = Frame::make(16, 16, 255, 0);
  for (int y = 0; y < 16; ++y) f.at(8, y) = 255;
  auto g = median3(f.view());
  for (auto v : g.data) EXPECT_EQ(v, 0);
}

TEST(Kernel, BoxBlurPreservesMean) {
  auto c = Frame::make(9, 9, 255, 77);
  EXPECT_EQ(box_blur(c.view(), 2).data, c.data);

  auto f = Frame::make(9, 9, 255, 0);
  f.at(4, 4) = 200;
  EXPECT_EQ(box_blur(f.view(), 0).data, f.data);
  auto b = box_blur(f.view(), 1);
  EXPECT_EQ(b.view().at(4, 4), 22);  // 200/9 rounded
  EXPECT_EQ(b.view().at(3, 3), 22);
  EXPECT_EQ(b.view().at(6, 6), 0);
}

TEST(Kernel, OtsuSeparatesTwoLevels) {
  auto f = Frame::make(20, 10, 255, 40);
  for (int y = 0; y < 10; ++y)
    for (int x = 10; x < 20; ++x) f.at(x, y) = 200;
  const auto t = otsu(f.view(), 0);
  EXPECT_GE(t, 40);
  EXPECT_LT(t, 200);
}

TEST(Kernel, OtsuIgnoresPixelsOutsideTheMask) {
  auto f = Frame::make(21, 21, 255, 40);
  for (int y = 0; y < 21; ++y)
    for (int x = 0; x < 21; ++x)
      if (x >= 10) f.at(x, y) = 200;
  // Corners (outside the disk) carry a bright value that must not matter.
  f.at(0, 0) = 255;
  f.at(20, 0) = 255;
  const auto t = otsu(f.view(), 9.0);
  EXPECT_GE(t, 40);
  EXPECT_LT(t, 200);
}

TEST(Kernel, OtsuOnConstantFrameReturnsThatValue) {
  auto f = Frame::make(8, 8, 255, 123);
  EXPECT_EQ(otsu(f.view(), 0), 123);
}

TEST(Kernel, ApplyDiskMaskSetsOutsideOnly) {
  auto f = Frame::make(21, 21, 255, 50);
  apply_disk_mask(f, 5.0, 0);
  EXPECT_EQ(f.view().at(10, 10), 50);
  EXPECT_EQ(f.view().at(0, 0), 0);
  EXPECT_EQ(f.view().at(20, 10), 0);
  auto g = Frame::make(21, 21, 255, 50);
  apply_disk_mask(g, 0.0, 0);  // no mask: no-op
  EXPECT_EQ(g.view().at(0, 0), 50);
}

TEST(Kernel, MedianInMask) {
  auto f = Frame::make(21, 21, 255, 10);
  f.at(0, 0) = 255;  // outside the mask
  EXPECT_EQ(median_in_mask(f.view(), 8.0), 10);
  auto g = Frame::make(5, 1, 255);
  for (int x = 0; x < 5; ++x) g.at(x, 0) = static_cast<std::uint16_t>(x * 10);
  EXPECT_EQ(median_in_mask(g.view(), 0), 20);
}

TEST(Kernel, ComponentsCountsAndFillsHoles) {
  const int w = 60, h = 60;
  auto m = disk_mask(w, h, 29.5, 29.5, 15, 9);
  auto filled = disk_mask(w, h, 29.5, 29.5, 15, 0);
  double expected = 0;
  for (auto v : filled) expected += v;
  auto cs = components(m, w, h, 0);
  ASSERT_EQ(cs.size(), 1U);
  EXPECT_NEAR(cs[0].area, expected, expected * 0.02);
  EXPECT_NEAR(cs[0].centroid.x, 29.5, 0.1);
  EXPECT_NEAR(cs[0].centroid.y, 29.5, 0.1);
  EXPECT_FALSE(cs[0].boundary.empty());
  EXPECT_EQ(static_cast<double>(cs[0].boundary.size()), cs[0].perimeter);
  EXPECT_EQ(cs[0].bbox.w, 30);  // center 29.5, r 15: columns 15..44
  // Filled disk perimeter is near 2*pi*r; an unfilled ring would roughly double it.
  EXPECT_LT(cs[0].perimeter, 2 * std::numbers::pi * 15 * 1.4);
}

TEST(Kernel, ComponentsSeparatesBlobsAndUses8Connectivity) {
  const int w = 20, h = 10;
  std::vector<std::uint8_t> m(200, 0);
  auto set = [&](int x, int y) { m[static_cast<std::size_t>(y * w + x)] = 1; };
  set(1, 1);
  set(2, 2);  // diagonal neighbours: one component
  set(10, 5);
  set(11, 5);
  auto cs = components(m, w, h, 0);
  ASSERT_EQ(cs.size(), 2U);
  EXPECT_EQ(cs[0].area, 2.0);
  EXPECT_EQ(cs[1].area, 2.0);
  EXPECT_NE(cs[0].label, cs[1].label);
}

TEST(Kernel, ComponentTouchesMaskEdge) {
  const int w = 61, h = 61;
  auto inner = disk_mask(w, h, 30, 30, 5, 0);
  auto cs = components(inner, w, h, 25.0);
  ASSERT_EQ(cs.size(), 1U);
  EXPECT_FALSE(cs[0].touches_mask_edge);

  auto edge = disk_mask(w, h, 52, 30, 4, 0);  // reaches radius 26 from the center
  auto ce = components(edge, w, h, 25.0);
  ASSERT_EQ(ce.size(), 1U);
  EXPECT_TRUE(ce[0].touches_mask_edge);
}

TEST(Kernel, FitCircleRecoversCircle) {
  std::vector<Vec2> pts;
  for (int i = 0; i < 32; ++i) {
    const double a = 2 * std::numbers::pi * i / 32;
    pts.push_back({12.3 + 5 * std::cos(a), 7.7 + 5 * std::sin(a)});
  }
  auto c = fit_circle(pts);
  EXPECT_NEAR(c.center.x, 12.3, 1e-6);
  EXPECT_NEAR(c.center.y, 7.7, 1e-6);
  EXPECT_NEAR(c.radius, 5.0, 1e-6);
  EXPECT_LT(c.rms, 1e-6);
}

TEST(Kernel, FitCircleOnCollinearPointsHasLargeRms) {
  std::vector<Vec2> pts;
  for (int i = 0; i < 10; ++i) pts.push_back({1.0 * i, 2.0 * i + 1});
  auto c = fit_circle(pts);
  EXPECT_TRUE(c.rms > 1.0 || !std::isfinite(c.radius));
  EXPECT_FALSE(std::isfinite(fit_circle({}).radius));  // empty input must not crash
}

TEST(Kernel, BoxBlurHugeRadiusIsTheFrameMean) {
  auto f = Frame::make(5, 4, 255, 0);
  for (int x = 0; x < 5; ++x) f.at(x, 0) = 100;  // mean = 500 / 20 = 25
  for (const int radius : {std::numeric_limits<int>::max(), std::numeric_limits<int>::max() - 1, 1000000}) {
    const Frame b = box_blur(f.view(), radius);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 5; ++x) EXPECT_EQ(b.view().at(x, y), 25) << radius;
  }
  // Non-positive radius stays an identity copy.
  const Frame same = box_blur(f.view(), std::numeric_limits<int>::min());
  EXPECT_EQ(same.data, f.data);
}
