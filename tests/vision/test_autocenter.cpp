#include <gtest/gtest.h>

#include <cmath>
#include <span>
#include <vector>

#include "pychron/vision/autocenter.hpp"
#include "sim_loop.hpp"

using namespace pychron::vision;
using namespace pychron::vision::sim;
using Action = AutocenterStep::Action;

namespace {

constexpr double kScale = 23.0;

struct LoopResult {
  AutocenterStep last;
  int calls = 0;
  int moves = 0;
};

// Drives the controller against a simulated stage until it stops asking for
// moves (or `max_calls` is hit). The stage applies each Move exactly.
LoopResult run_loop(Autocenter& ac, const HoleScene& scene, SimStage& stage, int max_calls = 8,
                    int frames = 3) {
  LoopResult r;
  std::uint64_t seq = 0;
  while (r.calls < max_calls) {
    auto set = render_frames(scene, stage, frames, seq);
    const auto views = set.views();
    r.last = ac.step(std::span<const FrameView>(views));
    ++r.calls;
    if (r.last.action != Action::Move) break;
    ++r.moves;
    stage.move(r.last.move_mm);
  }
  return r;
}

double dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

HoleScene scene_with_hole(Vec2 hole) {
  HoleScene s;
  s.hole_mm = hole;
  return s;
}

}  // namespace

TEST(Autocenter, ConvergesWithinThreeStepsFrom0p3mm) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, -0.2});
  scene.noise = 0.01;
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  SimStage stage{{0, 0}};
  const auto r = run_loop(ac, scene, stage, 4);
  EXPECT_EQ(r.last.action, Action::Converged);
  EXPECT_LE(r.moves, 3);
  EXPECT_LT(dist(stage.pos, scene.hole_mm), 0.03);
}

TEST(Autocenter, AlreadyCentredConvergesImmediately) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.5, 0.5});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  SimStage stage{{0.5, 0.5}};
  const auto r = run_loop(ac, scene, stage);
  EXPECT_EQ(r.last.action, Action::Converged);
  EXPECT_EQ(r.last.iteration, 0);
  EXPECT_EQ(r.calls, 1);
}

TEST(Autocenter, ClampsStepToMaxStep) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.9, 0});
  AutocenterParams p;
  p.crop_scale = 4.0;  // the default crop cannot see a hole 0.9 mm off centre
  p.max_total_mm = 5.0;
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto set = render_frames(scene, stage, 3, seq);
  const auto views = set.views();
  const auto s = ac.step(std::span<const FrameView>(views));
  ASSERT_EQ(s.action, Action::Move);
  EXPECT_NEAR(std::hypot(s.move_mm.x, s.move_mm.y), 0.5, 1e-9);
  EXPECT_GT(s.move_mm.x, 0);
  EXPECT_NEAR(s.offset_mm.x, 0.9, 0.05);
}

TEST(Autocenter, MedianRejectsOneBadFrame) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto set = render_frames(scene, stage, 3, seq);
  set.frames[1] = blank_frame(scene, 1);
  const auto views = set.views();
  const auto s = ac.step(std::span<const FrameView>(views));
  ASSERT_EQ(s.action, Action::Move);
  EXPECT_NEAR(s.move_mm.x, 0.3, 0.05);
  EXPECT_NEAR(s.move_mm.y, 0.0, 0.05);
}

TEST(Autocenter, MedianRejectsDisplacedFrame) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto set = render_frames(scene, stage, 3, seq);
  // One frame shows the hole centred (stage 0.3 mm away): offsets 0.3, 0, 0.3 mm.
  std::uint64_t seq2 = 1;
  set.frames[1] = render_frames(scene, SimStage{{0.3, 0}}, 1, seq2).frames[0];
  const auto views = set.views();
  const auto s = ac.step(std::span<const FrameView>(views));
  ASSERT_EQ(s.action, Action::Move);
  // The mean of three would be 0.2 mm; the median is the two agreeing frames.
  EXPECT_NEAR(s.move_mm.x, 0.3, 0.03);
}

TEST(Autocenter, FailsWhenNoTargetInMajority) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto set = render_frames(scene, stage, 3, seq);
  set.frames[0] = blank_frame(scene, 0);
  set.frames[1] = blank_frame(scene, 1);
  const auto views = set.views();
  const auto s = ac.step(std::span<const FrameView>(views));
  EXPECT_EQ(s.action, Action::Failed);
  EXPECT_EQ(s.reason, "no_target");
  EXPECT_EQ(s.move_mm.x, 0);
  EXPECT_EQ(s.move_mm.y, 0);
}

TEST(Autocenter, FailsOnWrongSignMap) {
  SimpleFinder finder;
  // Only the y sign is wrong, so the hole needs a nonzero y offset to run away.
  HoleScene scene = scene_with_hole({0.0, 0.15});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, false), kScale, {});
  SimStage stage{{0, 0}};
  const auto r = run_loop(ac, scene, stage, 4);
  EXPECT_EQ(r.last.action, Action::Failed);
  EXPECT_EQ(r.last.reason, "runaway");
  EXPECT_LE(r.calls, 4);
}

TEST(Autocenter, FailsAtIterationCap) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  AutocenterParams p;
  p.tolerance_mm = 1e-9;
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
  SimStage stage{{0, 0}};
  const auto r = run_loop(ac, scene, stage, 10);
  EXPECT_EQ(r.last.action, Action::Failed);
  EXPECT_EQ(r.last.reason, "max_iterations");
  EXPECT_EQ(r.calls, 5);
  EXPECT_EQ(r.moves, 4);
}

TEST(Autocenter, FailsWhenTotalMoveExceedsLimit) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  AutocenterParams p;
  p.max_total_mm = 0.1;
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
  SimStage stage{{0, 0}};
  const auto r = run_loop(ac, scene, stage);
  EXPECT_EQ(r.last.action, Action::Failed);
  EXPECT_EQ(r.last.reason, "max_total");
  EXPECT_EQ(r.last.move_mm.x, 0);
  EXPECT_EQ(r.last.move_mm.y, 0);
}

TEST(Autocenter, EmptySpanFails) {
  SimpleFinder finder;
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  const auto s = ac.step(std::span<const FrameView>{});
  EXPECT_EQ(s.action, Action::Failed);
  EXPECT_EQ(s.reason, "invalid");
  EXPECT_EQ(s.move_mm.x, 0);
  EXPECT_EQ(s.move_mm.y, 0);
}

TEST(Autocenter, InvalidMapOrRadiusFails) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto set = render_frames(scene, stage, 3, seq);
  const auto views = set.views();
  const std::span<const FrameView> sp(views);

  auto expect_invalid = [&](CameraStageMap map, double px, AutocenterParams p) {
    Autocenter ac(finder, map, px, p);
    const auto s = ac.step(sp);
    EXPECT_EQ(s.action, Action::Failed);
    EXPECT_EQ(s.reason, "invalid");
    EXPECT_EQ(s.move_mm.x, 0);
    EXPECT_EQ(s.move_mm.y, 0);
    EXPECT_TRUE(std::isfinite(s.offset_mm.x) && std::isfinite(s.offset_mm.y));
  };

  CameraStageMap singular;
  singular.m[0][0] = singular.m[0][1] = singular.m[1][0] = singular.m[1][1] = 0;
  const auto good = CameraStageMap::from_scale(kScale, false, true);
  expect_invalid(singular, kScale, {});
  AutocenterParams p;
  p.hole_radius_mm = 0;
  expect_invalid(good, kScale, p);
  p.hole_radius_mm = -0.5;
  expect_invalid(good, kScale, p);
  expect_invalid(good, 0.0, {});
  expect_invalid(good, std::nan(""), {});
}

TEST(Autocenter, ConvergesWithNeighboursNoiseAndCrosshair) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.25, -0.2});
  scene.neighbours = true;
  scene.glint = true;
  scene.crosshair = true;
  scene.noise = 0.01;
  {
    Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
    SimStage stage{{0, 0}};
    const auto r = run_loop(ac, scene, stage, 5);
    EXPECT_EQ(r.last.action, Action::Converged);
    EXPECT_LT(dist(stage.pos, scene.hole_mm), 0.03);
  }
  {
    // Aim point offset from the image centre: the hole ends up under the aim
    // point, not under the centre.
    AutocenterParams p;
    p.aim_offset_px = {10, -6};
    Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
    SimStage stage{{0, 0}};
    const auto r = run_loop(ac, scene, stage, 5);
    EXPECT_EQ(r.last.action, Action::Converged);
    const Vec2 want{scene.hole_mm.x - 10 / kScale, scene.hole_mm.y - 6 / kScale};
    EXPECT_LT(dist(stage.pos, want), 0.03);
  }
}

TEST(Autocenter, ResetAllowsReuse) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  AutocenterParams p;
  p.max_total_mm = 0.5;
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
  SimStage stage{{0, 0}};
  const auto first = run_loop(ac, scene, stage);
  ASSERT_EQ(first.last.action, Action::Converged);
  // Without reset the cumulative budget and iteration count carry over.
  ac.reset();
  SimStage stage2{{0, 0}};
  const auto second = run_loop(ac, scene, stage2);
  EXPECT_EQ(second.last.action, Action::Converged);
  EXPECT_LT(dist(stage2.pos, scene.hole_mm), 0.03);
  ASSERT_GE(second.calls, 1);
  EXPECT_EQ(second.calls, first.calls);
}

TEST(Autocenter, ClippedCropFails) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  AutocenterParams p;
  p.aim_offset_px = {80, 80};  // aim near the corner: the crop square leaves the frame
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto set = render_frames(scene, stage, 3, seq);
  const auto views = set.views();
  const auto s = ac.step(std::span<const FrameView>(views));
  EXPECT_EQ(s.action, Action::Failed);
  EXPECT_EQ(s.reason, "clipped");
  EXPECT_EQ(s.move_mm.x, 0);
  EXPECT_EQ(s.move_mm.y, 0);
  EXPECT_EQ(s.iteration, 0);
  // Not consumed: a controller with a fitting aim offset is unaffected, and the
  // clipped call itself did not advance the counter.
  p.aim_offset_px = {10, -6};
  Autocenter ok(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
  EXPECT_NE(ok.step(std::span<const FrameView>(views)).reason, "clipped");
}

TEST(Autocenter, ClippedDoesNotConsumeIteration) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  // Small frames: the default crop (59 px) does not fit in 50 px.
  HoleScene small = scene;
  small.width = small.height = 50;
  std::uint64_t seq = 0;
  auto bad = render_frames(small, SimStage{{0, 0}}, 3, seq);
  const auto bv = bad.views();
  EXPECT_EQ(ac.step(std::span<const FrameView>(bv)).reason, "clipped");
  auto good = render_frames(scene, SimStage{{0, 0}}, 3, seq);
  const auto gv = good.views();
  const auto s = ac.step(std::span<const FrameView>(gv));
  EXPECT_EQ(s.action, Action::Move);
  EXPECT_EQ(s.iteration, 0);
}

TEST(Autocenter, StaleFramesRejectedWithoutConsumingIteration) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, {});
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  auto first = render_frames(scene, stage, 3, seq);
  const auto v1 = first.views();
  const auto a = ac.step(std::span<const FrameView>(v1));
  ASSERT_EQ(a.action, Action::Move);
  EXPECT_EQ(a.iteration, 0);
  stage.move(a.move_mm);

  const auto stale = ac.step(std::span<const FrameView>(v1));
  EXPECT_EQ(stale.action, Action::Failed);
  EXPECT_EQ(stale.reason, "stale_frame");
  EXPECT_EQ(stale.move_mm.x, 0);
  EXPECT_EQ(stale.move_mm.y, 0);

  auto second = render_frames(scene, stage, 3, seq);
  const auto v2 = second.views();
  const auto b = ac.step(std::span<const FrameView>(v2));
  EXPECT_NE(b.action, Action::Failed);
  EXPECT_EQ(b.iteration, 1);
}

TEST(Autocenter, InvalidParamsFail) {
  SimpleFinder finder;
  HoleScene scene = scene_with_hole({0.3, 0});
  std::uint64_t seq = 0;
  auto set = render_frames(scene, SimStage{{0, 0}}, 3, seq);
  const auto views = set.views();
  const double nan = std::nan("");
  std::vector<AutocenterParams> bad(9);
  bad[0].tolerance_mm = -0.01;
  bad[1].tolerance_mm = nan;
  bad[2].max_step_mm = 0;
  bad[3].max_total_mm = nan;
  bad[4].max_iterations = 0;
  bad[5].crop_scale = nan;
  bad[6].aim_offset_px = {nan, 0};
  bad[7].crop_scale = 1e300;  // would overflow the int crop side
  bad[8].max_step_mm = nan;
  for (const auto& p : bad) {
    Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, true), kScale, p);
    const auto s = ac.step(std::span<const FrameView>(views));
    EXPECT_EQ(s.action, Action::Failed);
    EXPECT_EQ(s.reason, "invalid");
    EXPECT_EQ(s.move_mm.x, 0);
    EXPECT_EQ(s.move_mm.y, 0);
  }
}

TEST(Autocenter, ResetClearsRunawayAndStaleState) {
  SimpleFinder finder;
  // Wrong y sign: offsets grow. One growth is recorded before the reset.
  HoleScene scene = scene_with_hole({0.0, 0.15});
  Autocenter ac(finder, CameraStageMap::from_scale(kScale, false, false), kScale, {});
  SimStage stage{{0, 0}};
  std::uint64_t seq = 0;
  for (int i = 0; i < 2; ++i) {
    auto set = render_frames(scene, stage, 3, seq);
    const auto v = set.views();
    const auto s = ac.step(std::span<const FrameView>(v));
    ASSERT_EQ(s.action, Action::Move);
    stage.move(s.move_mm);
  }
  ac.reset();
  // Fresh run starting at a larger offset (0.4 mm) than the pre-reset one, with
  // timestamps restarting from the beginning. Stale state would fail it as
  // runaway or stale_frame.
  SimStage stage2{{0, -0.25}};
  std::uint64_t seq2 = 0;
  auto set = render_frames(scene, stage2, 3, seq2);
  const auto v = set.views();
  const auto s = ac.step(std::span<const FrameView>(v));
  EXPECT_EQ(s.action, Action::Move);
  EXPECT_EQ(s.iteration, 0);
}
