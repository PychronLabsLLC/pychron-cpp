#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "pychron/vision/spiral.hpp"

using namespace pychron::vision;

namespace {
double len(Vec2 p) { return std::hypot(p.x, p.y); }
}  // namespace

TEST(Spiral, HexagonFirstRingHasSixPointsAtBase) {
  Spiral s(SpiralKind::Hexagon, 0.5);
  std::vector<Vec2> ring;
  for (int i = 0; i < 6; ++i) ring.push_back(s.next());
  for (int i = 0; i < 6; ++i) {
    EXPECT_NEAR(len(ring[static_cast<std::size_t>(i)]), 0.5, 1e-12);
    const Vec2 a = ring[static_cast<std::size_t>(i)];
    const Vec2 b = ring[static_cast<std::size_t>((i + 1) % 6)];
    const double ang = std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
    EXPECT_NEAR(ang, std::numbers::pi / 3.0, 1e-12);
  }
  EXPECT_NEAR(ring[0].x, 0.5, 1e-12);  // starts at angle 0
  EXPECT_NEAR(ring[0].y, 0.0, 1e-12);
}

TEST(Spiral, HexagonSecondRingHasTwelvePoints) {
  Spiral s(SpiralKind::Hexagon, 0.5);
  for (int i = 0; i < 6; ++i) s.next();
  double max_len = 0;
  int at_vertex = 0, at_midpoint = 0;
  for (int i = 0; i < 12; ++i) {
    const double l = len(s.next());
    max_len = std::max(max_len, l);
    if (std::abs(l - 1.0) < 1e-12) ++at_vertex;
    if (std::abs(l - 1.0 * std::sqrt(3.0) / 2.0) < 1e-12) ++at_midpoint;
  }
  EXPECT_NEAR(max_len, 1.0, 1e-12);
  EXPECT_EQ(at_vertex, 6);
  EXPECT_EQ(at_midpoint, 6);
  // The 19th point starts ring 3 at angle 0.
  const Vec2 p = s.next();
  EXPECT_NEAR(p.x, 1.5, 1e-12);
  EXPECT_NEAR(p.y, 0.0, 1e-12);
}

TEST(Spiral, SquareGrowsByFactorPerLap) {
  Spiral s(SpiralKind::Square, 0.4, 1.1);
  double side = 0.4;
  for (int lap = 0; lap < 4; ++lap) {
    const Vec2 a = s.next(), b = s.next(), c = s.next(), d = s.next();
    EXPECT_NEAR(a.x, side, 1e-12);
    EXPECT_NEAR(a.y, 0, 1e-12);
    EXPECT_NEAR(b.x, 0, 1e-12);
    EXPECT_NEAR(b.y, side, 1e-12);
    EXPECT_NEAR(c.x, -side, 1e-12);
    EXPECT_NEAR(c.y, 0, 1e-12);
    EXPECT_NEAR(d.x, 0, 1e-12);
    EXPECT_NEAR(d.y, -side, 1e-12);
    side *= 1.1;
  }
}

TEST(Spiral, ResetRestartsSequence) {
  for (SpiralKind k : {SpiralKind::Hexagon, SpiralKind::Square}) {
    Spiral s(k, 0.5);
    std::vector<Vec2> first;
    for (int i = 0; i < 20; ++i) first.push_back(s.next());
    s.reset();
    for (int i = 0; i < 20; ++i) {
      const Vec2 p = s.next();
      EXPECT_EQ(p.x, first[static_cast<std::size_t>(i)].x);
      EXPECT_EQ(p.y, first[static_cast<std::size_t>(i)].y);
    }
  }
}

TEST(Spiral, Deterministic) {
  for (SpiralKind k : {SpiralKind::Hexagon, SpiralKind::Square}) {
    Spiral a(k, 0.3), b(k, 0.3);
    for (int i = 0; i < 100; ++i) {
      const Vec2 p = a.next(), q = b.next();
      EXPECT_EQ(p.x, q.x);
      EXPECT_EQ(p.y, q.y);
      EXPECT_GT(len(p), 0.0);  // never the anchor itself
    }
  }
}
