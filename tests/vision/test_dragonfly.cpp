#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
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
    if (!r.has_value()) {
      ADD_FAILURE() << "step returned an error: " << r.error().what;
      return DragonflyStep{};
    }
    const DragonflyStep d = *r;
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

// The aim point is image center + offset: a glow sitting there is on target,
// and one at the image center is off by the opposite of the offset.
TEST(Dragonfly, TracksTowardTheAimPointNotTheImageCenter) {
  // Image +x is stage +x, image +y is stage -y: 23 px right, 11.5 px up = (+1.0, +0.5) mm.
  DragonflyParams p = params();
  p.aim_offset_px = {23.0, -11.5};
  p.target_radius_mm = 1.0;  // crop wide enough to hold the aim point and the glow
  const Vec2 aim_mm{1.0, 0.5};
  // Glow at the aim point: nothing to correct.
  {
    Rig rig(glow_at(aim_mm), p);
    const auto d = rig.step();
    EXPECT_EQ(d.action, Action::Hold);
    EXPECT_EQ(d.reason, Reason::Deadband);
  }
  // Glow at the image center: the stage must move so the glow lands on the aim point.
  Rig rig(glow_at({0, 0}), p);
  for (int i = 0; i < 12; ++i) rig.step();
  EXPECT_LT(len({rig.stage.pos.x + aim_mm.x - rig.scene.glow_mm.x, rig.stage.pos.y + aim_mm.y - rig.scene.glow_mm.y}), 0.06);
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
  EXPECT_EQ(d.reason, Reason::Track);
  EXPECT_NEAR(len(d.target_mm), 0.5, 1e-9);
  EXPECT_GT(d.target_mm.x, 0.49);  // toward the glow
  EXPECT_NEAR(d.target_mm.y, 0.0, 0.03);
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
  // The scene is dark for the first steps; the first hexagon point (0.5, 0)
  // is only a search move, the glow is not there.
  Rig rig(glow_at({0.5, 0}), params());
  rig.scene.peak = 0.0;  // dark: no glow to see
  rig.scene.background = 0.02;
  for (int i = 0; i < 2; ++i) EXPECT_EQ(rig.step().reason, Reason::Miss);
  const auto s = rig.step();
  EXPECT_EQ(s.reason, Reason::Search);
  // Glow comes back off-center: track, which re-anchors and resets the spiral.
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
  std::vector<DragonflyStep> steps;
  for (int i = 0; i < 40; ++i) {
    const auto d = rig.step();
    steps.push_back(d);
    EXPECT_LE(len(d.target_mm), 2.5 + 1e-9);
    if (d.reason == Reason::PerimeterClamp) saw_clamp = true;
  }
  EXPECT_TRUE(saw_clamp);
  // Parked on the rim with the glow outside: hold, do not re-send the same move.
  for (std::size_t i = steps.size() - 5; i < steps.size(); ++i) {
    EXPECT_EQ(steps[i].action, Action::Hold);
    EXPECT_EQ(steps[i].reason, Reason::PerimeterClamp);
  }
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

TEST(Dragonfly, NonFiniteStartIsInvalid) {
  SimpleFinder finder;
  Dragonfly df(finder, good_map(), kScale, params());
  df.start(kT0, {std::numeric_limits<double>::quiet_NaN(), 0});
  const Frame f = Frame::make(200, 200, 255, 10);
  const FrameView v = f.view();
  auto r = df.step(std::span<const FrameView>(&v, 1), kT0 + std::chrono::seconds(1), {0, 0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Hold);
  EXPECT_EQ(r->reason, Reason::Invalid);
  EXPECT_TRUE(std::isfinite(r->target_mm.x));
  EXPECT_TRUE(std::isfinite(r->target_mm.y));
}

TEST(Dragonfly, NarrowBrightGlowTracks) {
  Rig rig(glow_at({0.2, 0}, 0.9), params());
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Move);
  EXPECT_EQ(d.reason, Reason::Track);
}

TEST(Dragonfly, GlowFillingMaskHoldsInsteadOfSearching) {
  for (auto [peak, sigma] : {std::pair{5.0, 0.6}, std::pair{2.0, 0.8}}) {
    GlowScene s = glow_at({0, 0}, peak);
    s.sigma_mm = sigma;
    Rig rig(s, params());
    for (int i = 0; i < 5; ++i) {
      const auto d = rig.step();
      EXPECT_EQ(d.action, Action::Hold) << "peak " << peak << " step " << i;
      EXPECT_EQ(d.reason, Reason::Saturated) << "peak " << peak << " step " << i;
    }
  }
}

TEST(Dragonfly, DarkFrameStillMissesThenSearches) {
  GlowScene s = glow_at({0, 0}, 0.0);
  Rig rig(s, params());
  EXPECT_EQ(rig.step().reason, Reason::Miss);
  EXPECT_EQ(rig.step().reason, Reason::Miss);
  EXPECT_EQ(rig.step().reason, Reason::Search);
}

TEST(Dragonfly, SearchIsBoundedAndRevisitsInterior) {
  Rig rig(glow_at({0, 0}, 0.0), params());
  int interior_late = 0;
  for (int i = 0; i < 300; ++i) {
    const auto d = rig.step();
    ASSERT_TRUE(std::isfinite(d.target_mm.x) && std::isfinite(d.target_mm.y));
    EXPECT_LE(len(d.target_mm), 2.5 + 1e-9);
    if (i >= 200 && d.action == Action::Move && len(d.target_mm) < 0.6 * 2.5) ++interior_late;
  }
  EXPECT_GT(interior_late, 0);
}

TEST(Dragonfly, HoldWithTargetReanchorsSpiral) {
  Rig rig(glow_at({0.5, 0}, 0.0), params());
  for (int i = 0; i < 2; ++i) EXPECT_EQ(rig.step().reason, Reason::Miss);
  const auto s = rig.step();  // search to (0.5, 0)
  ASSERT_EQ(s.reason, Reason::Search);
  rig.scene.peak = 0.6;  // glow is under the stage: deadband hold
  EXPECT_EQ(rig.step().reason, Reason::Deadband);
  rig.scene.peak = 0.0;
  for (int i = 0; i < 2; ++i) EXPECT_EQ(rig.step().reason, Reason::Miss);
  const auto s2 = rig.step();
  EXPECT_EQ(s2.reason, Reason::Search);
  EXPECT_NEAR(s2.target_mm.x, 1.0, 1e-9);  // first ring point about the new position
  EXPECT_NEAR(s2.target_mm.y, 0.0, 1e-9);
}

TEST(Dragonfly, YSignIsPinned) {
  {
    Rig rig(glow_at({0, 0.2}), params());
    const auto d = rig.step();
    EXPECT_EQ(d.action, Action::Move);
    EXPECT_NEAR(d.target_mm.y, 0.2, 0.03);
  }
  {
    Rig rig(glow_at({0, -0.2}), params());
    const auto d = rig.step();
    EXPECT_EQ(d.action, Action::Move);
    EXPECT_NEAR(d.target_mm.y, -0.2, 0.03);
  }
}

TEST(Dragonfly, ClampedCropGivesCorrectOffset) {
  DragonflyParams p = params();
  p.target_radius_mm = 3.0;  // crop side exceeds the frame: clamped to it
  Rig rig(glow_at({0.2, 0.1}), p);
  const auto d = rig.step();
  EXPECT_EQ(d.action, Action::Move);
  EXPECT_NEAR(d.target_mm.x, 0.2, 0.03);
  EXPECT_NEAR(d.target_mm.y, 0.1, 0.03);
}

TEST(Dragonfly, ProjectedSearchPointsStillAdvance) {
  DragonflyParams p = params();
  p.perimeter_radius_mm = 0.3;  // every ring-1 point (0.5 mm) is projected
  p.miss_frames_before_search = 1;
  Rig rig(glow_at({0, 0}, 0.0), p);
  std::vector<Vec2> pts;
  for (int i = 0; i < 6; ++i) {
    const auto d = rig.step();
    EXPECT_EQ(d.action, Action::Move);
    EXPECT_EQ(d.reason, Reason::PerimeterClamp);
    EXPECT_NEAR(len(d.target_mm), 0.3, 1e-9);
    pts.push_back(d.target_mm);
  }
  // Ring 1 lies wholly outside, so the spiral restarts after six points, but
  // within the ring the six directions differ.
  for (std::size_t i = 0; i < pts.size(); ++i)
    for (std::size_t j = i + 1; j < pts.size(); ++j)
      EXPECT_GT(len({pts[i].x - pts[j].x, pts[i].y - pts[j].y}), 0.1);
  // The first three are the hexagon directions 0, 60 and 120 degrees at R.
  for (int i = 0; i < 3; ++i) {
    const double a = i * std::numbers::pi / 3.0;
    EXPECT_NEAR(pts[static_cast<std::size_t>(i)].x, 0.3 * std::cos(a), 1e-9);
    EXPECT_NEAR(pts[static_cast<std::size_t>(i)].y, 0.3 * std::sin(a), 1e-9);
  }
  // After the ring the search restarts at ring 1's first point.
  const auto again = rig.step();
  EXPECT_NEAR(again.target_mm.x, pts[0].x, 1e-9);
  EXPECT_NEAR(again.target_mm.y, pts[0].y, 1e-9);
}

TEST(Dragonfly, OverflowingOffsetGivesInvalid) {
  SimpleFinder finder;
  Dragonfly df(finder, good_map(), kScale, params());
  df.start(kT0, {-1e308, 0});
  auto r = df.step({}, kT0 + std::chrono::seconds(1), {1e308, 0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->action, Action::Hold);
  EXPECT_EQ(r->reason, Reason::Invalid);
  EXPECT_TRUE(std::isfinite(r->target_mm.x) && std::isfinite(r->target_mm.y));
}

TEST(Dragonfly, SquareSearchStaysFiniteAndInside) {
  DragonflyParams p = params();
  p.spiral = SpiralKind::Square;
  Rig rig(glow_at({0, 0}, 0.0), p);
  for (int i = 0; i < 2000; ++i) {
    const auto d = rig.step();
    ASSERT_TRUE(std::isfinite(d.target_mm.x) && std::isfinite(d.target_mm.y));
    ASSERT_LE(len(d.target_mm), 2.5 + 1e-9);
  }
}

TEST(Dragonfly, StaleBoundaries) {
  SimpleFinder finder;
  GlowScene scene = glow_at({0.2, 0});
  const TimePoint t1 = kT0 + std::chrono::seconds(1);
  Frame lit = render(scene, {0, 0}).first;
  lit.timestamp = kT0;  // old, but no Move has happened yet
  const FrameView lv = lit.view();
  Dragonfly df(finder, good_map(), kScale, params());
  df.start(kT0, {0, 0});
  auto r = df.step(std::span<const FrameView>(&lv, 1), t1, {0, 0});
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->action, Action::Move);

  Frame blank = blank_frame(scene);
  blank.timestamp = t1;  // equal to the Move time: accepted
  const FrameView bv = blank.view();
  Frame old = blank;
  old.timestamp = t1 - std::chrono::milliseconds(1);
  const FrameView ov = old.view();

  Dragonfly ref(finder, good_map(), kScale, params());
  ref.start(kT0, {0, 0});
  ASSERT_TRUE(ref.step(std::span<const FrameView>(&lv, 1), t1, {0, 0}).has_value());

  const TimePoint t2 = t1 + std::chrono::seconds(1);
  auto bad = df.step(std::span<const FrameView>(&ov, 1), t2, r->target_mm);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().kind, ErrorKind::Protocol);
  for (int i = 0; i < 3; ++i) {
    auto a = df.step(std::span<const FrameView>(&bv, 1), t2, r->target_mm);
    auto b = ref.step(std::span<const FrameView>(&bv, 1), t2, r->target_mm);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->action, b->action);
    EXPECT_EQ(a->reason, b->reason);
    EXPECT_EQ(a->target_mm.x, b->target_mm.x);
    EXPECT_EQ(a->target_mm.y, b->target_mm.y);
  }
}

TEST(Dragonfly, BadParametersGiveInvalid) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<DragonflyParams> bad;
  auto add = [&](auto mutate) {
    DragonflyParams p = params();
    mutate(p);
    bad.push_back(p);
  };
  add([&](DragonflyParams& p) { p.perimeter_radius_mm = 0; });
  add([&](DragonflyParams& p) { p.max_step_mm = -1; });
  add([&](DragonflyParams& p) { p.spiral_base_mm = nan; });
  add([&](DragonflyParams& p) { p.target_radius_mm = 0; });
  add([&](DragonflyParams& p) { p.aggressiveness = -0.1; });
  add([&](DragonflyParams& p) { p.move_threshold_mm = nan; });
  add([&](DragonflyParams& p) { p.saturation_threshold = 1.5; });
  add([&](DragonflyParams& p) { p.saturation_threshold = 0; });
  add([&](DragonflyParams& p) { p.aim_offset_px = {nan, 0}; });
  add([&](DragonflyParams& p) { p.aim_offset_px = {0, 4294967306.0}; });
  add([&](DragonflyParams& p) { p.aim_offset_px = {1e300, 0}; });
  add([&](DragonflyParams& p) { p.miss_frames_before_search = 0; });
  for (const DragonflyParams& p : bad) {
    SimpleFinder finder;
    Dragonfly df(finder, good_map(), kScale, p);
    df.start(kT0, {0, 0});
    auto r = df.step({}, kT0 + std::chrono::seconds(1), {0.1, 0});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->action, Action::Hold);
    EXPECT_EQ(r->reason, Reason::Invalid);
    EXPECT_TRUE(std::isfinite(r->target_mm.x) && std::isfinite(r->target_mm.y));
  }
  SimpleFinder finder;
  Dragonfly df(finder, good_map(), kScale, params());
  df.start(kT0, {0, 0});
  auto r = df.step({}, kT0 + std::chrono::seconds(1), {nan, 0});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->reason, Reason::Invalid);
  auto r2 = Dragonfly(finder, good_map(), nan, params());
  r2.start(kT0, {0, 0});
  auto r3 = r2.step({}, kT0 + std::chrono::seconds(1), {0, 0});
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(r3->reason, Reason::Invalid);
}
