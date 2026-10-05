#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>
#include <system_error>
#include <vector>

#include "pychron/vision/finder.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/legacy_finder.hpp"
#include "pychron/vision/opencv_source.hpp"
#include "pychron/vision/pgm.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron::vision;

namespace {

double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

FinderParams hole_params(const HoleScene& s) {
  FinderParams p;
  p.mode = FinderMode::Hole;
  p.expected_radius_px = s.hole_radius_mm * s.px_per_mm;
  p.mask_radius_px = std::min(s.width, s.height) / 2.0;
  return p;
}

FinderParams glow_params(const GlowScene& s) {
  FinderParams p;
  p.mode = FinderMode::Glow;
  p.expected_radius_px = s.sigma_mm * s.px_per_mm;
  p.mask_radius_px = std::min(s.width, s.height) / 2.0;
  return p;
}

// Ten stage offsets that keep the target within the legacy center gate.
std::vector<Vec2> offsets() {
  std::vector<Vec2> o;
  for (int i = 0; i < 10; ++i) o.push_back({0.04 * i - 0.18, 0.03 * (i % 4) - 0.05});
  return o;
}

// Within the tight crop: the hole stays clear of the frame edge.
std::vector<Vec2> hole_offsets() {
  std::vector<Vec2> o;
  for (int i = 0; i < 10; ++i) o.push_back({0.01 * i - 0.045, 0.008 * ((i * 3) % 7) - 0.024});
  return o;
}

}  // namespace

TEST(LegacyFinder, FactoryMatchesBuildFlag) { EXPECT_EQ(make_legacy_finder() != nullptr, opencv_enabled()); }

TEST(LegacyFinder, AgreesWithSimpleOnSyntheticHole) {
  auto legacy = make_legacy_finder();
  if (!legacy) GTEST_SKIP() << "built without OpenCV";
  // The legacy autocenter looks at a crop of ceil(2.55 x radius) pixels, so the hole fills
  // about half of it; its 0.25..0.75 white-fraction limit rejects every threshold on a
  // frame where the hole is a small part of the image.
  HoleScene s;
  s.width = s.height = static_cast<int>(std::ceil(2.55 * s.hole_radius_mm * s.px_per_mm));
  SimpleFinder simple;
  for (const Vec2 off : hole_offsets()) {
    auto [f, truth] = render(s, off);
    const auto a = simple.find(f.view(), hole_params(s));
    const auto b = legacy->find(f.view(), hole_params(s));
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty()) << "stage " << off.x << "," << off.y;
    EXPECT_LT(dist(a[0].center_px, b[0].center_px), 2.0) << "stage " << off.x << "," << off.y;
  }
}

TEST(LegacyFinder, AgreesWithSimpleOnSyntheticGlow) {
  auto legacy = make_legacy_finder();
  if (!legacy) GTEST_SKIP() << "built without OpenCV";
  GlowScene s;
  SimpleFinder simple;
  for (const Vec2 off : offsets()) {
    auto [f, truth] = render(s, off);
    const auto a = simple.find(f.view(), glow_params(s));
    const auto b = legacy->find(f.view(), glow_params(s));
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty()) << "stage " << off.x << "," << off.y;
    EXPECT_LT(dist(a[0].center_px, b[0].center_px), 2.0) << "stage " << off.x << "," << off.y;
  }
}

TEST(LegacyFinder, UniformFramesGiveNoTarget) {
  auto legacy = make_legacy_finder();
  if (!legacy) GTEST_SKIP() << "built without OpenCV";
  for (const auto mode : {FinderMode::Hole, FinderMode::Glow}) {
    for (const std::uint16_t level : {0, 100, 255}) {
      const Frame f = Frame::make(120, 120, 255, level);
      FinderParams p;
      p.mode = mode;
      p.expected_radius_px = 11;
      p.mask_radius_px = 60;
      EXPECT_TRUE(legacy->find(f.view(), p).empty()) << "level " << level;
    }
  }
}

TEST(LegacyFinder, SaturationUsesLegacyDenominator) {
  auto legacy = make_legacy_finder();
  if (!legacy) GTEST_SKIP() << "built without OpenCV";
  constexpr int kDepth = 255;
  constexpr double r = 10;
  Frame f = Frame::make(120, 120, kDepth, 0);
  for (int y = 0; y < f.height; ++y)
    for (int x = 0; x < f.width; ++x)
      if (std::hypot(x - 60.0, y - 60.0) <= r) f.at(x, y) = kDepth;
  FinderParams p;
  p.mode = FinderMode::Glow;
  p.expected_radius_px = 11;
  p.mask_radius_px = 60;
  const auto t = legacy->find(f.view(), p);
  ASSERT_FALSE(t.empty());
  // sum / ((area + perimeter / 2) * depth). Area and perimeter are the finder's own polygon
  // measures (the perimeter recovered from circularity = 4 pi area / perimeter^2); sum is
  // the pixel count of the disk at full depth.
  double sum = 0;
  for (int y = 0; y < f.height; ++y)
    for (int x = 0; x < f.width; ++x) sum += f.view().at(x, y);
  ASSERT_GT(t[0].circularity, 0);
  const double area = t[0].area_px;
  const double perimeter = std::sqrt(4 * std::numbers::pi * area / t[0].circularity);
  const double expected = sum / ((area + perimeter / 2) * kDepth);
  EXPECT_NEAR(t[0].score, expected, 0.02 * expected);
  EXPECT_DOUBLE_EQ(saturation(t[0]), t[0].score);
}

TEST(LegacyFinder, RealFixtureComparison) {
  auto legacy = make_legacy_finder();
  if (!legacy) GTEST_SKIP() << "built without OpenCV";
  std::vector<std::filesystem::path> dirs;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(PYCHRON_VISION_DATA_DIR, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code ec2;
    if (it->is_directory(ec2) && std::filesystem::exists(it->path() / "case.toml", ec2)) dirs.push_back(it->path());
  }
  std::sort(dirs.begin(), dirs.end());
  ASSERT_FALSE(dirs.empty());

  // A report, not a gate: every frame runs, including those marked skip, and
  // only crashes and non-finite results fail.
  auto describe = [&](const std::vector<Target>& t, const FixtureFrame& ff, double tol) {
    if (t.empty()) return std::string("none");
    EXPECT_TRUE(std::isfinite(t[0].center_px.x) && std::isfinite(t[0].center_px.y));
    EXPECT_TRUE(std::isfinite(t[0].radius_px) && std::isfinite(t[0].score));
    if (!ff.center_px) return std::string("unmarked");
    const double e = dist(t[0].center_px, *ff.center_px);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f%s", e, e <= tol ? " ok" : "");
    return std::string(buf);
  };
  // Extra column, holes only: the legacy autocenter looks at a crop of ceil(2.55 x radius)
  // pixels, which the fixtures are not. This re-runs LegacyFinder on such a crop centered on
  // the marked center, to show whether the crop alone explains a miss. Diagnostic only.
  std::printf("%-12s %-5s %-4s %-14s %-14s %-16s\n", "case", "mode", "skip", "SimpleFinder", "LegacyFinder",
              "Legacy tight crop");
  for (const auto& dir : dirs) {
    auto c = load_case(dir);
    ASSERT_TRUE(c.has_value());
    for (const auto& ff : c->frames) {
      auto fr = read_pgm(c->dir / ff.file);
      ASSERT_TRUE(fr.has_value());
      FinderParams p;
      p.mode = c->mode;
      p.expected_radius_px = c->expected_radius_px;
      p.mask_radius_px = std::min(fr->width, fr->height) / 2.0;
      const auto a = SimpleFinder{}.find(fr->view(), p);
      const auto b = legacy->find(fr->view(), p);
      std::string tight = "-";
      if (c->mode == FinderMode::Hole && ff.center_px) {
        const int side = static_cast<int>(std::ceil(2.55 * c->expected_radius_px));
        const Rect r{static_cast<int>(std::lround(ff.center_px->x - (side - 1) / 2.0)),
                     static_cast<int>(std::lround(ff.center_px->y - (side - 1) / 2.0)), side, side};
        const Frame cropped = crop(fr->view(), r);
        FinderParams cp = p;
        cp.mask_radius_px = side / 2.0;
        auto t = legacy->find(cropped.view(), cp);
        for (auto& x : t) {
          x.center_px.x += r.x;
          x.center_px.y += r.y;
        }
        tight = describe(t, ff, c->tolerance_px);
      }
      std::printf("%-12s %-5s %-4s %-14s %-14s %-16s\n", c->dir.filename().string().c_str(),
                  c->mode == FinderMode::Glow ? "glow" : "hole", ff.skip ? "yes" : "no",
                  describe(a, ff, c->tolerance_px).c_str(), describe(b, ff, c->tolerance_px).c_str(), tight.c_str());
    }
  }
}

TEST(OpenCvSource, StubReportsConfigErrorWhenDisabled) {
  if (opencv_enabled()) GTEST_SKIP() << "built with OpenCV";
  const auto r = open_opencv_source("0", SourceConfig{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
  EXPECT_NE(r.error().what.find("built without OpenCV"), std::string::npos);
}

TEST(OpenCvSource, MissingFileIsIoError) {
  if (!opencv_enabled()) GTEST_SKIP() << "built without OpenCV";
  const auto r = open_opencv_source("/nonexistent/pychron_no_such_video.avi", SourceConfig{});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Io);
}

TEST(OpenCvSource, BadRotationIsConfigError) {
  if (!opencv_enabled()) GTEST_SKIP() << "built without OpenCV";
  SourceConfig c;
  c.rotate = 45;
  const auto r = open_opencv_source("/nonexistent/pychron_no_such_video.avi", c);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, pychron::ErrorKind::Config);
}
