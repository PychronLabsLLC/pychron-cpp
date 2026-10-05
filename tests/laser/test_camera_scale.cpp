// The camera's pixel scale measured by jogging the stage (live camera
// design, section 4).

#include <cmath>
#include <numbers>

#include <gtest/gtest.h>

#include "laser_harness.hpp"
#include "pychron/laser/camera_scale.hpp"

using namespace pychron;
using namespace pychron::laser;
using namespace pychron::laser::harness;

namespace {

// What a fixed target does in the picture when the stage moves by (dx, dy)
// under a camera of `scale` px/mm, mirrored as told and turned by `turn`.
vision::JogPair jog(double dx, double dy, double scale, bool flip_x, bool flip_y, double turn_deg = 0, double stretch_y = 1) {
  // the map from pixels to the move that centres them, inverted: a stage
  // move d shifts the picture by -M^-1 d
  const double a = turn_deg * std::numbers::pi / 180.0;
  double px = -(flip_x ? -1 : 1) * dx * scale;
  double py = -(flip_y ? -1 : 1) * dy * scale * stretch_y;
  return {{dx, dy}, {px * std::cos(a) - py * std::sin(a), px * std::sin(a) + py * std::cos(a)}};
}

struct ScaleRig : CameraHarness {
  using CameraHarness::CameraHarness;
  Result<void> arrive() {
    auto ended = drive();
    if (!ended) return fail(ended.error());
    for (int i = 0; i < 5; ++i) advance();  // settled
    return {};
  }
  Result<ScaleMeasurement> measure(double step = 0.5) {
    return measure_camera_scale(system, step, [this] { return arrive(); });
  }
  void on_hole(const char* hole) {
    ASSERT_TRUE(system.move_to_position(hole, false));
    ASSERT_TRUE(arrive());
  }
};

}  // namespace

TEST(ScaleFrom, ReadsScaleAndFlipsOffTwoJogs) {
  for (const bool flip_x : {false, true}) {
    for (const bool flip_y : {false, true}) {
      const std::vector<vision::JogPair> pairs{jog(0.5, 0, 31, flip_x, flip_y), jog(0, 0.5, 31, flip_x, flip_y)};
      const auto m = scale_from(pairs);
      ASSERT_TRUE(m) << m.error().what;
      EXPECT_NEAR(m->px_per_mm, 31, 1e-9);
      EXPECT_EQ(m->flip_x, flip_x);
      EXPECT_EQ(m->flip_y, flip_y);
      EXPECT_NEAR(m->skew_deg, 0, 1e-9);
      EXPECT_NEAR(m->anisotropy, 0, 1e-9);
      // the same map the typed values would have given
      const auto typed = vision::CameraStageMap::from_scale(31, flip_x, flip_y);
      for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) EXPECT_NEAR(m->map.m[i][j], typed.m[i][j], 1e-12);
    }
  }
}

TEST(ScaleFrom, ACameraTurnedOnItsMountIsStillAMap) {
  const std::vector<vision::JogPair> pairs{jog(0.5, 0, 20, false, true, 12), jog(0, 0.5, 20, false, true, 12)};
  const auto m = scale_from(pairs);
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_NEAR(m->px_per_mm, 20, 1e-9);
  EXPECT_NEAR(m->skew_deg, 0, 1e-9);
  // a target 10 px right of the aim: the move that centres it is not along x alone
  const vision::Vec2 move = m->map.to_mm({10, 0});
  EXPECT_NEAR(std::hypot(move.x, move.y), 0.5, 1e-9);
  EXPECT_GT(std::abs(move.y), 0.05);
}

TEST(ScaleFrom, RefusesAxesThatDisagree) {
  const std::vector<vision::JogPair> stretched{jog(0.5, 0, 20, false, true), jog(0, 0.5, 20, false, true, 0, 1.08)};
  auto m = scale_from(stretched);
  ASSERT_FALSE(m);
  EXPECT_NE(m.error().what.find("scale"), std::string::npos) << m.error().what;
  // one sighting was of another hole: the axes are no longer square
  std::vector<vision::JogPair> skewed{jog(0.5, 0, 20, false, true), jog(0, 0.5, 20, false, true)};
  // (turned 5 degrees, its length kept)
  const vision::Vec2 d = skewed[1].image_delta_px;
  const double a = 5 * std::numbers::pi / 180;
  skewed[1].image_delta_px = {d.x * std::cos(a) - d.y * std::sin(a), d.x * std::sin(a) + d.y * std::cos(a)};
  m = scale_from(skewed);
  ASSERT_FALSE(m);
  EXPECT_NE(m.error().what.find("square"), std::string::npos) << m.error().what;
  // and a target that did not move at all tells nothing
  const std::vector<vision::JogPair> still{{{0.5, 0}, {0, 0}}, {{0, 0.5}, {0, 0}}};
  EXPECT_FALSE(scale_from(still));
}

TEST(CameraScaleStore, KeepsAMeasurementPerDevice) {
  LabDir lab;
  const CameraScaleStore store(lab.dir / "camera_scales");
  auto none = store.load("co2");
  ASSERT_TRUE(none) << none.error().what;
  EXPECT_FALSE(none->has_value());
  const auto m = scale_from(std::vector<vision::JogPair>{jog(0.5, 0, 31.25, true, true), jog(0, 0.5, 31.25, true, true)});
  ASSERT_TRUE(m);
  ASSERT_TRUE(store.save("co2", *m));
  EXPECT_TRUE(fs::exists(lab.dir / "camera_scales" / "co2.toml"));
  auto back = store.load("co2");
  ASSERT_TRUE(back) << back.error().what;
  ASSERT_TRUE(back->has_value());
  EXPECT_NEAR((*back)->px_per_mm, 31.25, 1e-9);
  EXPECT_TRUE((*back)->flip_x);
  for (int i = 0; i < 2; ++i)
    for (int j = 0; j < 2; ++j) EXPECT_DOUBLE_EQ((*back)->map.m[i][j], m->map.m[i][j]);
  EXPECT_FALSE(store.load("diode")->has_value());
  EXPECT_FALSE(store.save("../co2", *m));
  ASSERT_TRUE(store.clear("co2"));
  EXPECT_FALSE(store.load("co2")->has_value());
  EXPECT_TRUE(store.clear("co2")) << "nothing to clear is not an error";
  // a file somebody broke is said, not guessed at
  std::ofstream(lab.dir / "camera_scales" / "co2.toml") << "m = [[1, 2]]\n";
  EXPECT_FALSE(store.load("co2"));
}

TEST(MeasureScale, FindsTheSimulatedCamerasScaleAndFlips) {
  for (const bool flip_x : {false, true}) {
    CameraConfig real = camera_config();
    real.px_per_mm = 23;
    real.flip_x = flip_x;
    // what the system was told is wrong on every count
    CameraConfig told = real;
    told.px_per_mm = 36;  // roughly right is all it has to be
    told.flip_x = !flip_x;
    told.flip_y = !real.flip_y;
    ScaleRig rig(told, real);
    rig.on_hole("3");
    const auto start = rig.at();
    const auto m = rig.measure();
    ASSERT_TRUE(m) << m.error().what;
    EXPECT_NEAR(m->px_per_mm, 23, 0.5);
    EXPECT_EQ(m->flip_x, real.flip_x);
    EXPECT_EQ(m->flip_y, real.flip_y);
    EXPECT_NEAR(rig.at().x, start.x, 1e-6) << "the stage is put back";
    EXPECT_NEAR(rig.at().y, start.y, 1e-6);
  }
}

TEST(MeasureScale, AMeasuredScaleIsWhatCentringUses) {
  CameraConfig real = camera_config();
  CameraConfig told = real;
  told.flip_x = true;  // a wrong sign: centring would run away from the hole
  told.flip_y = false;
  {
    ScaleRig wrong(told, real);
    ASSERT_TRUE(wrong.system.move_to_position("3", true));
    ASSERT_TRUE(wrong.drive());
    EXPECT_NE(wrong.system.last_autocenter().result, AutocenterOutcome::Result::Converged);
  }
  ScaleRig rig(told, real);
  rig.on_hole("3");
  const auto m = rig.measure();
  ASSERT_TRUE(m) << m.error().what;
  rig.system.set_measured_scale(*m);
  ASSERT_TRUE(rig.system.move_to_position("3", true));
  ASSERT_TRUE(rig.drive());
  EXPECT_EQ(rig.system.last_autocenter().result, AutocenterOutcome::Result::Converged);
  EXPECT_NEAR(rig.at().x, 15.15, 0.04);
  EXPECT_NEAR(rig.at().y, 19.90, 0.04);
  // and what the window draws is to the measured scale
  EXPECT_NEAR(rig.system.view()->px_per_mm, 23, 0.5);
}

TEST(MeasureScale, NeedsSomethingToSeeAndASaneStep) {
  ScaleRig rig;
  ASSERT_TRUE(rig.system.set_xy(-30, -30));  // nowhere near a hole
  ASSERT_TRUE(rig.arrive());
  const auto blind = rig.measure();
  ASSERT_FALSE(blind);
  EXPECT_NE(blind.error().what.find("nothing"), std::string::npos) << blind.error().what;
  EXPECT_NEAR(rig.at().x, -30, 1e-6);
  rig.on_hole("3");
  EXPECT_FALSE(rig.measure(0));
  EXPECT_FALSE(rig.measure(5));
  LaserHarness no_camera;
  const auto none = measure_camera_scale(no_camera.system, 0.25, [] { return Result<void>{}; });
  ASSERT_FALSE(none);
  EXPECT_NE(none.error().what.find("camera"), std::string::npos) << none.error().what;
}

// A step too small to see in the picture measures the picture's noise.
TEST(MeasureScale, AStepThePictureDoesNotShowIsRefused) {
  ScaleRig rig;
  rig.on_hole("3");
  const auto m = rig.measure(0.02);  // under half a pixel at 23 px/mm
  ASSERT_FALSE(m);
  EXPECT_NE(m.error().what.find("pixels"), std::string::npos) << m.error().what;
}

// A camera that does not follow the stage (one for looking, over a
// simulated laser): the target does not move, and that is not a scale.
TEST(MeasureScale, APictureThatDoesNotFollowTheStageIsRefused) {
  LaserHarness h;
  CameraConfig config = camera_config();
  config.sim_noise = 0;
  // always the view from hole 3, wherever the stage goes
  const TraySightFn fixed = [sight = h.system.sight()] {
    TraySight seen = sight();
    seen.stage = {15, 20};
    return seen;
  };
  ASSERT_TRUE(h.system.attach_viewer(config, std::make_unique<SimTrayCamera>(config, fixed, h.clock), h.clock));
  ASSERT_TRUE(h.system.set_tray("small"));
  const auto arrive = [&]() -> Result<void> {
    for (int i = 0; i < 4000; ++i) {
      auto moving = h.system.moving();
      if (!moving) return fail(moving.error());
      if (!*moving) return {};
      h.advance();
    }
    return fail(ErrorKind::Timeout, "still moving");
  };
  const auto m = measure_camera_scale(h.system, 0.25, arrive);
  ASSERT_FALSE(m);
  EXPECT_NE(m.error().what.find("pixels"), std::string::npos) << m.error().what;
}

// A step that takes the target past half way to its neighbour: the nearest
// thing in the picture is then the neighbour.
TEST(MeasureScale, AStepThatReachesTheNeighbourIsRefused) {
  LaserHarness h;
  CameraConfig config = camera_config();
  config.sim_noise = 0;
  // the picture moves twelve times as far as the stage: 0.25 mm looks like 3 mm, on a tray of 5 mm pitch
  const TraySightFn magnified = [sight = h.system.sight()] {
    TraySight seen = sight();
    seen.stage = {15 + (seen.stage.x - 15) * 12, 20 + (seen.stage.y - 20) * 12};
    return seen;
  };
  ASSERT_TRUE(h.system.attach_viewer(config, std::make_unique<SimTrayCamera>(config, magnified, h.clock), h.clock));
  ASSERT_TRUE(h.system.set_tray("small"));
  const auto arrive = [&]() -> Result<void> {
    for (int i = 0; i < 4000; ++i) {
      auto moving = h.system.moving();
      if (!moving) return fail(moving.error());
      if (!*moving) return {};
      h.advance();
    }
    return fail(ErrorKind::Timeout, "still moving");
  };
  ASSERT_TRUE(h.system.move_to_position("3", false));
  ASSERT_TRUE(arrive());
  const auto m = measure_camera_scale(h.system, 0.25, arrive);
  ASSERT_FALSE(m) << "px/mm " << m->px_per_mm;
}

TEST(MeasureScale, AMeasurementRemembersWhatCameraItWasOf) {
  ScaleRig rig;
  rig.on_hole("3");
  const auto m = rig.measure();
  ASSERT_TRUE(m) << m.error().what;
  EXPECT_EQ(m->geometry, rig.camera.geometry());
  EXPECT_FALSE(m->geometry.empty());
  const CameraScaleStore store(rig.lab.dir / "camera_scales");
  ASSERT_TRUE(store.save("co2", *m));
  EXPECT_EQ((*store.load("co2"))->geometry, m->geometry);
  // three sightings, one to spare: how well they agree is known
  EXPECT_LT(m->map.residual_mm, 0.02);
}

TEST(MeasureScale, AStageThatFailsPartWayIsSaid) {
  ScaleRig rig;
  rig.on_hole("3");
  int arrivals = 0;
  const auto m = measure_camera_scale(rig.system, 0.25, [&]() -> Result<void> {
    if (++arrivals == 2) return fail(ErrorKind::Io, "limit switch");
    return rig.arrive();
  });
  ASSERT_FALSE(m);
  EXPECT_NE(m.error().what.find("limit switch"), std::string::npos) << m.error().what;
}
