#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <span>
#include <vector>

#include "pychron/vision/dragonfly.hpp"
#include "sim_loop.hpp"

using namespace pychron::vision;
using namespace pychron::vision::sim;
using Action = DragonflyStep::Action;
using Reason = DragonflyStep::Reason;
using pychron::ErrorKind;
using pychron::TimePoint;

namespace {

constexpr double kScale = 23.0;
const TimePoint kT0{};

CameraStageMap good_map() { return CameraStageMap::from_scale(kScale, false, true); }

DragonflyParams params() {
  DragonflyParams p;
  p.total_duration = std::chrono::hours(1);
  return p;
}

double len(Vec2 v) { return std::hypot(v.x, v.y); }

// Drives the controller against a simulated stage; `stage` is an absolute
// position and each Move is applied exactly (start is the origin).
struct Rig {
  SimpleFinder finder;
  GlowScene scene;
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  Dragonfly df;

  Rig(GlowScene s, DragonflyParams p, CameraStageMap map = good_map())
      : scene(s), df(finder, map, kScale, p) {
    df.start(kT0, stage.pos);
  }

  // One frame per step; `now` is the newest frame's timestamp.
  DragonflyStep step(int n = 1) {
    auto set = render_frames(scene, stage, n, seq);
    const auto views = set.views();
    const TimePoint now = set.frames.back().timestamp;
    auto r = df.step(std::span<const FrameView>(views), now, stage.pos);
    EXPECT_TRUE(r.has_value());
    const DragonflyStep d = r.value();
    if (d.action == Action::Move) stage.pos = d.target_mm;
    return d;
  }
};

GlowScene glow_at(Vec2 mm, double peak = 0.6) {
  GlowScene s;
  s.glow_mm = mm;
  s.peak = peak;
  return s;
}

}  // namespace

TEST(Dragonfly, TracksDriftingGlow) {
  Rig rig(glow_at({0, 0}), params());
  for (int i = 1; i <= 40; ++i) {
    rig.scene.glow_mm = {0.02 * i, 0};
    rig.step();
  }
  EXPECT_LT(len({rig.scene.glow_mm.x - rig.stage.pos.x, rig.scene.glow_mm.y - rig.stage.pos.y}), 0.05);
}

TEST(Dragonfly, HoldsWhenSaturated) {
  GlowScene s = glow_at({0.2, 0}, 2.0);
  s.sigma_mm = 0.6;
  Rig rig(s, params());
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Hold);
  EXPECT_EQ(d.reason, Reason::Saturated);
  EXPECT_GE(d.saturation, 0.75);
}

TEST(Dragonfly, MovesOnPureXDrift) {
  Rig rig(glow_at({0.1, 0}), params());
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Move);
  EXPECT_EQ(d.reason, Reason::Track);
  EXPECT_NEAR(d.target_mm.x, 0.1, 0.03);
  EXPECT_NEAR(d.target_mm.y, 0.0, 0.03);
}

TEST(Dragonfly, DeadbandBelowThreshold) {
  Rig rig(glow_at({0.02, 0}), params());
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Hold);
  EXPECT_EQ(d.reason, Reason::Deadband);
}

TEST(Dragonfly, ClampsStepToMaxStep) {
  DragonflyParams p = params();
  p.target_radius_mm = 1.0;  // wide enough crop to see a 1.2 mm offset
  Rig rig(glow_at({1.2, 0}), p);
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Move);
  EXPECT_NEAR(len(d.target_mm), 0.5, 1e-9);
}

TEST(Dragonfly, AggressivenessScalesMove) {
  DragonflyParams p = params();
  p.aggressiveness = 0.5;
  Rig rig(glow_at({0.2, 0}), p);
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Move);
  EXPECT_NEAR(d.target_mm.x, 0.1, 0.02);
}

TEST(Dragonfly, SearchesOnlyAfterNMisses) {
  Rig rig(glow_at({0, 0}), params());
  const Frame blank = blank_frame(rig.scene);
  std::vector<std::pair<Action, Reason>> seq;
  for (int i = 0; i < 3; ++i) {
    const FrameView v = blank.view();
    auto r = rig.df.step(std::span<const FrameView>(&v, 1), kT0 + std::chrono::milliseconds(10 * (i + 1)),
                         {0, 0});
    ASSERT_TRUE(r.has_value());
    seq.emplace_back(r->action, r->reason);
  }
  ASSERT_EQ(seq.size(), 3u);
  EXPECT_EQ(seq[0], std::make_pair(Action::Hold, Reason::Miss));
  EXPECT_EQ(seq[1], std::make_pair(Action::Hold, Reason::Miss));
  EXPECT_EQ(seq[2], std::make_pair(Action::Move, Reason::Search));
}

TEST(Dragonfly, ReacquiresAfterSearchAndResetsSpiral) {
  // Glow sits at 0.5 mm; the first search point (0.5, 0) is at the base
  // distance, so the stage lands on it.
  Rig rig(glow_at({0.5, 0}), params());
  rig.scene.peak = 0.0;  // dark: no glow to see
  rig.scene.background = 0.02;
  for (int i = 0; i < 2; ++i) EXPECT_EQ(rig.step().reason, Reason::Miss);
  const auto s = rig.step();
  EXPECT_EQ(s.reason, Reason::Search);
  // Glow comes back off-centre: track, which re-anchors and resets the spiral.
  rig.scene.peak = 0.6;
  rig.scene.glow_mm = {rig.stage.pos.x + 0.2, rig.stage.pos.y};
  const auto t = rig.step();
  EXPECT_EQ(t.reason, Reason::Track);
  const Vec2 anchor = t.target_mm;
  // Lose it again: after N misses the search restarts at ring 1, around the
  // new anchor.
  rig.scene.peak = 0.0;
  for (int i = 0; i < 2; ++i) EXPECT_EQ(rig.step().reason, Reason::Miss);
  const auto s2 = rig.step();
  EXPECT_EQ(s2.reason, Reason::Search);
  EXPECT_NEAR(s2.target_mm.x - anchor.x, 0.5, 1e-9);
  EXPECT_NEAR(s2.target_mm.y - anchor.y, 0.0, 1e-9);
}

TEST(Dragonfly, NeverLeavesPerimeter) {
  DragonflyParams p = params();
  p.target_radius_mm = 1.0;  // crop wide enough to see the glow from the rim
  p.miss_frames_before_search = 1;
  Rig rig(glow_at({4.0, 0}), p);
  bool saw_clamp = false;
  for (int i = 0; i < 30; ++i) {
    const auto d = rig.step();
    EXPECT_LE(len(d.target_mm), 2.5 + 1e-9);
    if (d.reason == Reason::PerimeterClamp) saw_clamp = true;
  }
  EXPECT_TRUE(saw_clamp);
}

TEST(Dragonfly, DoneAtDuration) {
  DragonflyParams p = params();
  p.total_duration = std::chrono::seconds(10);
  SimpleFinder finder;
  Dragonfly df(finder, good_map(), kScale, p);
  df.start(kT0, {1.0, 2.0});
  auto r = df.step({}, kT0 + std::chrono::seconds(10), {1.5, 2.0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Done);
  EXPECT_EQ(r->reason, Reason::Elapsed);
  EXPECT_NEAR(r->target_mm.x, 0.5, 1e-12);
}

TEST(Dragonfly, StaleFrameIsAnError) {
  Rig rig(glow_at({0.2, 0}), params());
  const auto d = rig.step();  // Move; decision time = frame 0's timestamp
  ASSERT_EQ(d.action, Action::Move);
  Frame old = blank_frame(rig.scene, 0);
  old.timestamp = kT0 + std::chrono::milliseconds(1);
  const FrameView v = old.view();
  auto r = rig.df.step(std::span<const FrameView>(&v, 1), kT0 + std::chrono::seconds(5), rig.stage.pos);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
}

TEST(Dragonfly, EmptySpanIsAMiss) {
  SimpleFinder finder;
  Dragonfly df(finder, good_map(), kScale, params());
  df.start(kT0, {0, 0});
  auto r = df.step({}, kT0 + std::chrono::seconds(1), {0, 0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Hold);
  EXPECT_EQ(r->reason, Reason::Miss);
}

TEST(Dragonfly, InvalidMapGivesInvalid) {
  CameraStageMap singular;
  singular.m[0][0] = 1;
  singular.m[0][1] = 2;
  singular.m[1][0] = 2;
  singular.m[1][1] = 4;
  SimpleFinder finder;
  Dragonfly df(finder, singular, kScale, params());
  df.start(kT0, {0, 0});
  auto r = df.step({}, kT0 + std::chrono::seconds(1), {0.3, 0.1});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Hold);
  EXPECT_EQ(r->reason, Reason::Invalid);
  EXPECT_TRUE(std::isfinite(r->target_mm.x));
  EXPECT_TRUE(std::isfinite(r->target_mm.y));
}

TEST(Dragonfly, StepBeforeStartIsInvalid) {
  SimpleFinder finder;
  Dragonfly df(finder, good_map(), kScale, params());
  auto r = df.step({}, kT0 + std::chrono::seconds(1), {0, 0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Hold);
  EXPECT_EQ(r->reason, Reason::Invalid);
}

TEST(Dragonfly, SixteenBitGlowBehavesAsEightBit) {
  auto run = [](std::uint16_t depth) {
    GlowScene s = glow_at({0, 0});
    s.pixel_depth = depth;
    Rig rig(s, params());
    std::vector<std::pair<Action, Reason>> seq;
    for (int i = 1; i <= 10; ++i) {
      rig.scene.glow_mm = {0.05 * i, 0.02 * i};
      const auto d = rig.step();
      seq.emplace_back(d.action, d.reason);
    }
    return seq;
  };
  EXPECT_EQ(run(255), run(65535));
}

TEST(Dragonfly, AllZeroSaturationDoesNotDivideByZero) {
  // Stub finder reporting a target of score 0: weights all vanish.
  struct ZeroFinder final : ITargetFinder {
    std::vector<Target> find(const FrameView&, const FinderParams&, FinderDebug*) override {
      Target t;
      t.center_px = {50, 60};
      t.score = 0;
      return {t};
    }
  } finder;
  Dragonfly df(finder, good_map(), kScale, params());
  df.start(kT0, {0, 0});
  const Frame f = Frame::make(200, 200, 255, 10);
  const FrameView v = f.view();
  auto r = df.step(std::span<const FrameView>(&v, 1), kT0 + std::chrono::seconds(1), {0, 0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Move);
  EXPECT_TRUE(std::isfinite(r->target_mm.x));
  EXPECT_TRUE(std::isfinite(r->target_mm.y));
  EXPECT_GT(len(r->target_mm), 0.0);
}
