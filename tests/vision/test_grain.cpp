#include "pychron/vision/grain.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace std::chrono_literals;
using pychron::vision::GrainModel;
using pychron::vision::GrainParams;
using pychron::vision::GrainTruth;
using pychron::vision::kGrainTick;
using pychron::vision::Vec2;

namespace {

// Runs `seconds` in whole ticks, one call each, the beam still.
void run(GrainModel& model, double seconds, Vec2 beam, double output) {
  const auto ticks = static_cast<long>(std::llround(seconds / 0.01));
  for (long i = 0; i < ticks; ++i) model.advance(kGrainTick, beam, output);
}

bool same(const GrainTruth& a, const GrainTruth& b) {
  return a.center_mm.x == b.center_mm.x && a.center_mm.y == b.center_mm.y && a.heat == b.heat &&
         a.brightness == b.brightness;
}

// Everything on, for the determinism test.
GrainParams busy() {
  GrainParams p;
  p.flicker = 0.2;
  p.flares_per_s = 1;
  p.dropouts_per_s = 0.2;
  p.creep_mm_per_s = {0.01, -0.01};
  p.wander_mm_per_rt_s = 0.05;
  p.jumps_per_s = 0.5;
  return p;
}

// A beam so wide the grain stays under it wherever it goes.
GrainParams wide() {
  GrainParams p;
  p.beam_radius_mm = 100;
  return p;
}

constexpr Vec2 kOrigin{0, 0};

}  // namespace

TEST(GrainModel, SameSeedSameTruth) {
  GrainModel a(busy(), 1, 0, kOrigin), b(busy(), 1, 0, kOrigin), c(busy(), 2, 0, kOrigin);
  run(a, 30, kOrigin, 20);
  run(b, 30, kOrigin, 20);
  run(c, 30, kOrigin, 20);
  EXPECT_TRUE(same(a.truth(), b.truth()));
  EXPECT_FALSE(same(a.truth(), c.truth()));
}

TEST(GrainModel, FrameRateDoesNotMatter) {
  GrainModel a(busy(), 1, 0, kOrigin), b(busy(), 1, 0, kOrigin);
  a.advance(1s, kOrigin, 20);
  for (int i = 0; i < 10; ++i) b.advance(100ms, kOrigin, 20);
  EXPECT_TRUE(same(a.truth(), b.truth()));
}

TEST(GrainModel, RemainderOfATickCarries) {
  GrainModel a(busy(), 1, 0, kOrigin), b(busy(), 1, 0, kOrigin);
  for (int i = 0; i < 3; ++i) a.advance(4ms, kOrigin, 20);
  b.advance(12ms, kOrigin, 20);
  EXPECT_TRUE(same(a.truth(), b.truth()));
  EXPECT_EQ(a.tick(), 1);
  EXPECT_EQ(b.tick(), 1);
  a.advance(8ms, kOrigin, 20);  // the 2 ms left and 8 more: another tick
  EXPECT_EQ(a.tick(), 2);
}

TEST(GrainModel, DefaultsAreOneSteadyLobe) {
  GrainModel m(GrainParams{}, 1, 0, kOrigin);
  run(m, 20, kOrigin, 20);
  const auto t = m.truth();
  EXPECT_NEAR(t.heat, 1.0, 1e-6);
  EXPECT_NEAR(t.brightness, 0.6, 1e-6);
  EXPECT_EQ(t.center_mm.x, 0);
  EXPECT_EQ(t.center_mm.y, 0);
  const auto lobes = m.lobes();
  ASSERT_EQ(lobes.size(), 1U);
  EXPECT_EQ(lobes[0].at_mm.x, 0);
  EXPECT_EQ(lobes[0].sigma_mm, 0.3);
  EXPECT_NEAR(lobes[0].peak, 0.6, 1e-6);
}

TEST(GrainModel, HeatRisesAndFallsByItsLags) {
  GrainModel m(GrainParams{}, 1, 0, kOrigin);
  run(m, 0.5, kOrigin, 20);  // heat_tau_s
  EXPECT_NEAR(m.truth().heat, 1 - std::exp(-1.0), 0.01);
  run(m, 20, kOrigin, 20);
  m.advance(kGrainTick, {10, 0}, 20);  // one tick lands the beam far away
  const double start = m.truth().heat;
  run(m, 1.0, {10, 0}, 20);  // cool_tau_s
  EXPECT_NEAR(m.truth().heat, start * std::exp(-1.0), 0.01);
}

TEST(GrainModel, GlowFollowsHeatSteeply) {
  GrainModel m(GrainParams{}, 1, 0, kOrigin);
  run(m, 20, kOrigin, 10);
  EXPECT_NEAR(m.truth().heat, 0.5, 1e-6);
  EXPECT_NEAR(m.truth().brightness, 0.6 * 0.0625, 1e-6);
}

TEST(GrainModel, ColdGrainDoesNotMove) {
  GrainParams p = busy();
  GrainModel m(p, 1, 0, {0.2, -0.1});
  run(m, 60, {0.2, -0.1}, 0);
  EXPECT_EQ(m.truth().center_mm.x, 0.2);
  EXPECT_EQ(m.truth().center_mm.y, -0.1);
}

TEST(GrainModel, CreepsAtItsRateWhenHot) {
  GrainParams p = wide();
  p.heat_tau_s = 0.01;
  p.creep_mm_per_s = {0.01, 0};
  GrainModel m(p, 1, 0, kOrigin);
  run(m, 1, kOrigin, 20);
  const Vec2 before = m.truth().center_mm;
  for (int i = 0; i < 1000; ++i) m.advance(kGrainTick, m.truth().center_mm, 20);
  EXPECT_NEAR(m.truth().center_mm.x - before.x, 0.1, 0.002);
  EXPECT_NEAR(m.truth().center_mm.y - before.y, 0.0, 1e-9);
}

TEST(GrainModel, EventRatesMatchTheirKeys) {
  {
    GrainParams p;
    p.flares_per_s = 2;
    p.flare_gain = 3;
    GrainModel m(p, 7, 0, kOrigin);
    run(m, 30, kOrigin, 20);  // settled
    long flares = 0;
    double last = m.truth().brightness;
    for (int i = 0; i < 100000; ++i) {
      m.advance(kGrainTick, kOrigin, 20);
      const double now = m.truth().brightness;
      if (now > last + 1e-6) ++flares;
      last = now;
    }
    const double mean = 2.0 * 1000, sigma = std::sqrt(100000 * 0.02 * 0.98);
    EXPECT_NEAR(static_cast<double>(flares), mean, 3 * sigma);
  }
  {
    GrainParams p = wide();
    p.jumps_per_s = 1;
    p.jump_mm = 0.3;
    GrainModel m(p, 7, 0, kOrigin);
    run(m, 30, kOrigin, 20);
    long jumps = 0;
    double total = 0;
    Vec2 last = m.truth().center_mm;
    for (int i = 0; i < 100000; ++i) {
      m.advance(kGrainTick, last, 20);
      const Vec2 now = m.truth().center_mm;
      const double d = std::hypot(now.x - last.x, now.y - last.y);
      if (d > 0) {
        ++jumps;
        total += d;
      }
      last = now;
    }
    const double sigma = std::sqrt(100000 * 0.01 * 0.99);
    EXPECT_NEAR(static_cast<double>(jumps), 1000.0, 3 * sigma);
    ASSERT_GT(jumps, 0);
    EXPECT_NEAR(total / static_cast<double>(jumps), 0.3, 3 * 0.3 / std::sqrt(static_cast<double>(jumps)));
  }
}

TEST(GrainModel, ADropoutIsDarkForItsTime) {
  GrainParams p;
  p.dropouts_per_s = 0.1;
  p.dropout_s = 0.5;
  GrainModel m(p, 3, 0, kOrigin);
  run(m, 30, kOrigin, 20);
  long dark = 0;
  const long ticks = 30000;  // 300 s
  for (long i = 0; i < ticks; ++i) {
    m.advance(kGrainTick, kOrigin, 20);
    if (m.truth().brightness == 0) ++dark;
  }
  const double dark_s = static_cast<double>(dark) * 0.01;
  // Events: Poisson with mean 30 in 300 s; each costs dropout_s of dark.
  const double expect = 0.1 * 0.5 * 300, sigma = std::sqrt(0.1 * 300) * 0.5;
  EXPECT_NEAR(dark_s, expect, 3 * sigma);
}

TEST(GrainModel, StaysInItsHole) {
  GrainParams p = wide();
  p.jumps_per_s = 5;
  p.jump_mm = 5;
  p.wander_mm_per_rt_s = 1;
  GrainModel m(p, 4, 0.5, kOrigin);
  for (int i = 0; i < 30000; ++i) {
    m.advance(kGrainTick, m.truth().center_mm, 20);
    const Vec2 c = m.truth().center_mm;
    ASSERT_LE(std::hypot(c.x, c.y), 0.5 + 1e-12);
  }
}

TEST(GrainModel, AHoleOfNoSizeDoesNotClamp) {
  GrainParams p = wide();
  p.jumps_per_s = 5;
  GrainModel m(p, 4, 0, {0.3, 0});
  for (int i = 0; i < 3000; ++i) m.advance(kGrainTick, m.truth().center_mm, 20);
  EXPECT_NE(m.truth().center_mm.x, 0.3);
}

TEST(GrainModel, IdleHoursCostNothing) {
  const auto t0 = std::chrono::steady_clock::now();
  GrainModel bulk(busy(), 5, 0, kOrigin), stepped(busy(), 5, 0, kOrigin);
  bulk.advance(10h, {50, 0}, 0);
  const auto took = std::chrono::steady_clock::now() - t0;
  EXPECT_LT(took, 5s);
  EXPECT_EQ(bulk.tick(), 3600000);
  for (long i = 0; i < 3600000; ++i) stepped.advance(kGrainTick, {50, 0}, 0);
  EXPECT_EQ(stepped.tick(), 3600000);
  run(bulk, 30, kOrigin, 20);
  run(stepped, 30, kOrigin, 20);
  EXPECT_EQ(bulk.truth().heat, stepped.truth().heat);
  EXPECT_EQ(bulk.truth().center_mm.x, stepped.truth().center_mm.x);
  EXPECT_EQ(bulk.truth().center_mm.y, stepped.truth().center_mm.y);
}

TEST(GrainModel, BadInputChangesNothing) {
  GrainModel m(busy(), 1, 0, kOrigin);
  run(m, 5, kOrigin, 20);
  const GrainTruth before = m.truth();
  const auto tick = m.tick();
  m.advance(0ms, kOrigin, 20);
  m.advance(-1s, kOrigin, 20);
  m.advance(kGrainTick, {std::nan(""), 0}, 20);
  m.advance(kGrainTick, {0, std::numeric_limits<double>::infinity()}, 20);
  EXPECT_TRUE(same(before, m.truth()));
  EXPECT_EQ(tick, m.tick());
}

namespace {

double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

TEST(GrainModel, LobesShareBrightness) {
  GrainParams p = busy();
  p.lobes = 5;
  p.lobe_spread_mm = 0.2;
  p.lobe_wander_mm_per_rt_s = 0.1;
  GrainModel m(p, 3, 0, kOrigin);
  for (int i = 0; i < 3000; ++i) {
    m.advance(kGrainTick, m.truth().center_mm, 20);
    double sum = 0;
    for (const auto& l : m.lobes()) sum += l.peak;
    ASSERT_NEAR(sum, m.truth().brightness, 1e-9) << i;
  }
}

TEST(GrainModel, LobesStayNearTheGrain) {
  GrainParams p = wide();
  p.lobes = 6;
  p.lobe_spread_mm = 0.2;
  p.lobe_wander_mm_per_rt_s = 1;
  p.wander_mm_per_rt_s = 0.05;
  GrainModel m(p, 5, 0, kOrigin);
  for (int i = 0; i < 30000; ++i) {
    m.advance(kGrainTick, m.truth().center_mm, 20);
    for (const auto& l : m.lobes()) ASSERT_LE(dist(l.at_mm, m.truth().center_mm), 0.4 + 1e-12) << i;
  }
}

TEST(GrainModel, LobesMoveTheCenterOfLightNotTheGrain) {
  GrainParams p = wide();
  p.lobes = 4;
  p.lobe_spread_mm = 0.3;
  p.lobe_life_s = 1;
  GrainModel m(p, 9, 0, kOrigin);
  run(m, 20, kOrigin, 20);
  auto light = [&] {
    Vec2 c{};
    double w = 0;
    for (const auto& l : m.lobes()) {
      c.x += l.peak * l.at_mm.x;
      c.y += l.peak * l.at_mm.y;
      w += l.peak;
    }
    return Vec2{c.x / w, c.y / w};
  };
  const Vec2 first = light();
  double most = 0;
  for (int i = 0; i < 6000; ++i) {
    m.advance(kGrainTick, kOrigin, 20);
    most = std::max(most, dist(light(), first));
    ASSERT_EQ(m.truth().center_mm.x, 0);
    ASSERT_EQ(m.truth().center_mm.y, 0);
  }
  EXPECT_GT(most, 0.05);
}

TEST(GrainModel, ALobeDiesAndReturnsElsewhere) {
  GrainParams p = wide();
  p.lobe_spread_mm = 0.3;
  p.lobe_life_s = 2;
  GrainModel m(p, 2, 0, kOrigin);
  run(m, 20, kOrigin, 20);
  Vec2 before = m.lobes()[0].at_mm;
  bool dark = false, returned = false;
  for (int i = 0; i < 20000 && !returned; ++i) {
    m.advance(kGrainTick, kOrigin, 20);
    const auto l = m.lobes()[0];
    if (!dark) {
      if (l.peak == 0) dark = true;
      else before = l.at_mm;
    } else if (l.peak > 0.9 * m.truth().brightness) {
      returned = true;
      EXPECT_GT(dist(l.at_mm, before), 1e-6);
    }
  }
  EXPECT_TRUE(dark);
  EXPECT_TRUE(returned);
}

namespace {

GrainParams sized() {
  GrainParams p;
  p.grain_radius_mm = 0.5;
  p.beam_radius_mm = 0.2;
  return p;
}

bool same_outline(const std::vector<Vec2>& a, const std::vector<Vec2>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].x != b[i].x || a[i].y != b[i].y) return false;
  return true;
}

double heat_after(const GrainParams& p, Vec2 beam, double seconds = 5) {
  GrainModel m(p, 1, 0, kOrigin);
  run(m, seconds, beam, 20);
  return m.truth().heat;
}

}  // namespace

TEST(GrainShape, TheSeedFixesTheOutline) {
  GrainModel a(sized(), 1, 0, kOrigin), b(sized(), 1, 0, kOrigin), c(sized(), 2, 0, kOrigin);
  EXPECT_FALSE(a.outline().empty());
  EXPECT_TRUE(same_outline(a.outline(), b.outline()));
  EXPECT_FALSE(same_outline(a.outline(), c.outline()));
}

TEST(GrainShape, ItsAreaIsTheRadiusGiven) {
  for (double irregularity : {0.0, 0.6}) {
    GrainParams p = sized();
    p.grain_irregularity = irregularity;
    GrainModel m(p, 7, 0, kOrigin);
    EXPECT_NEAR(m.truth().radius_mm, 0.5, 1e-9);
    const auto& o = m.outline();
    double area = 0;
    for (std::size_t i = 0; i < o.size(); ++i) {
      const Vec2 a = o[i], b = o[(i + 1) % o.size()];
      area += a.x * b.y - b.x * a.y;
    }
    EXPECT_NEAR(std::abs(area) / 2, std::numbers::pi * 0.25, 1e-9);
  }
}

TEST(GrainShape, ItHasTheCornersAsked) {
  for (int sides : {3, 6, 16}) {
    GrainParams p = sized();
    p.grain_sides = sides;
    GrainModel m(p, 1, 0, kOrigin);
    EXPECT_EQ(m.outline().size(), static_cast<std::size_t>(sides) * 8);
  }
}

TEST(GrainShape, APointGrainHasNoOutline) {
  GrainModel m(GrainParams{}, 1, 0, kOrigin);
  EXPECT_TRUE(m.outline().empty());
  EXPECT_FALSE(m.body().has_value());
  EXPECT_EQ(m.truth().radius_mm, 0);
}

TEST(GrainShape, ABeamAnywhereOnItHeatsAsCentered) {
  GrainModel probe(sized(), 3, 0, kOrigin);
  const Vec2 q = probe.outline()[5];
  const double centered = heat_after(sized(), kOrigin);
  const double inside = heat_after(sized(), {0.3 * q.x, 0.3 * q.y});
  EXPECT_NEAR(inside, centered, 1e-6);
  (void)q;
}

TEST(GrainShape, ABeamPastTheEdgeHeatsLess) {
  GrainModel probe(sized(), 1, 0, kOrigin);
  const Vec2 q = probe.outline()[5];
  EXPECT_LT(heat_after(sized(), {2 * q.x, 2 * q.y}), heat_after(sized(), kOrigin) - 0.05);
}

TEST(GrainShape, LobesStayWithinTheRadius) {
  GrainParams p = sized();
  p.lobes = 3;
  p.lobe_wander_mm_per_rt_s = 2;
  GrainModel m(p, 1, 0, kOrigin);
  for (int i = 0; i < 6000; ++i) {
    m.advance(kGrainTick, kOrigin, 20);
    for (const auto& l : m.lobes()) {
      const Vec2 c = m.truth().center_mm;
      EXPECT_LE(std::hypot(l.at_mm.x - c.x, l.at_mm.y - c.y), 0.5 + 1e-9);
    }
  }
}

TEST(GrainShape, ItStaysInsideItsHole) {
  GrainParams p = sized();
  p.creep_mm_per_s = {0.05, 0.03};
  GrainModel m(p, 1, 1.0, kOrigin);
  for (int i = 0; i < 12000; ++i) {
    m.advance(kGrainTick, m.truth().center_mm, 20);
    const Vec2 c = m.truth().center_mm;
    for (const Vec2& o : m.outline()) EXPECT_LE(std::hypot(c.x + o.x, c.y + o.y), 1.0 + 1e-9);
  }
  EXPECT_GT(std::hypot(m.truth().center_mm.x, m.truth().center_mm.y), 0.1);
}

TEST(GrainShape, AGrainBiggerThanItsHoleSitsAtTheCenter) {
  GrainParams p = sized();
  p.creep_mm_per_s = {0.5, 0.5};
  GrainModel m(p, 1, 0.2, {0.1, 0.1});
  run(m, 10, kOrigin, 20);
  const GrainTruth t = m.truth();
  EXPECT_EQ(t.center_mm.x, 0);
  EXPECT_EQ(t.center_mm.y, 0);
  EXPECT_TRUE(std::isfinite(t.heat) && std::isfinite(t.brightness));
  for (const auto& l : m.lobes()) EXPECT_TRUE(std::isfinite(l.at_mm.x) && std::isfinite(l.at_mm.y));
}

TEST(GrainShape, BodyAndLobesShareTheBrightness) {
  GrainParams p = sized();
  p.lobes = 3;
  GrainModel m(p, 1, 0, kOrigin);
  run(m, 5, kOrigin, 20);
  const double b = m.truth().brightness;
  ASSERT_GT(b, 0);
  ASSERT_TRUE(m.body().has_value());
  double sum = m.body()->peak;
  for (const auto& l : m.lobes()) sum += l.peak;
  EXPECT_NEAR(sum, b, 1e-9 * b);
  EXPECT_NEAR(m.body()->peak, 0.4 * b, 1e-9 * b);
}
