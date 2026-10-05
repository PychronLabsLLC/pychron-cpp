// The simulated camera: the tray as a camera looking down the beam path
// would see it (laser autocenter design, section 4).

#include "pychron/laser/tray_camera.hpp"

#include <chrono>
#include <filesystem>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/clock.hpp"
#include "pychron/vision/finder.hpp"
#include "pychron/vision/fixture.hpp"
#include "pychron/vision/synth.hpp"

using namespace pychron;
using namespace pychron::laser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

// Holes 5 mm apart on a line, calibrated at stage (10, 20), (15, 20), (20, 20).
TraySight sight_at(double x, double y) {
  TraySight s;
  s.stage = {x, y};
  s.holes = {{10, 20}, {15, 20}, {20, 20}};
  s.hole_radius_mm = 0.5;
  return s;
}

CameraConfig config(double ex = 0, double ey = 0) {
  CameraConfig c;
  c.device = "co2";
  c.sim_tray_error_mm = {ex, ey};
  c.sim_noise = 0;
  return c;
}

// Where the finder sees a hole, as an offset in px from the image centre.
std::optional<vision::Vec2> seen(const vision::Frame& frame, double px_per_mm = 23.0) {
  vision::SimpleFinder finder;
  vision::FinderParams params;
  params.mode = vision::FinderMode::Hole;
  params.expected_radius_px = 0.5 * px_per_mm;
  // the hole nearest the centre
  std::optional<vision::Vec2> best;
  for (const auto& t : finder.find(frame.view(), params)) {
    const vision::Vec2 off{t.center_px.x - (frame.width - 1) / 2.0, t.center_px.y - (frame.height - 1) / 2.0};
    if (!best || std::hypot(off.x, off.y) < std::hypot(best->x, best->y)) best = off;
  }
  return best;
}

}  // namespace

TEST(SimTrayCamera, TheHoleIsWhereTheTruthSays) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);  // on the middle hole, as calibrated
  SimTrayCamera camera(config(0.2, -0.1), [&] { return sight; }, clock);
  auto frame = camera.grab();
  ASSERT_TRUE(frame) << frame.error().what;
  EXPECT_EQ(frame->width, 200);
  EXPECT_EQ(frame->height, 200);
  // the real hole is 0.2 right and 0.1 below (stage -y is image +y)
  const auto off = seen(*frame);
  ASSERT_TRUE(off);
  EXPECT_NEAR(off->x, 0.2 * 23, 1.0);
  EXPECT_NEAR(off->y, 0.1 * 23, 1.0);
  const auto truth = camera.last_truth();
  EXPECT_TRUE(truth.visible);
  EXPECT_NEAR(truth.center_px.x - 99.5, 0.2 * 23, 1e-6);
  EXPECT_NEAR(truth.center_px.y - 99.5, 0.1 * 23, 1e-6);
  EXPECT_NEAR(truth.radius_px, 11.5, 1e-9);
}

TEST(SimTrayCamera, FollowsTheStage) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  ASSERT_TRUE(camera.grab());
  EXPECT_NEAR(camera.last_truth().center_px.x, 99.5, 1e-6);
  sight.stage = {15.5, 20.25};  // the stage moves right and up: the hole goes left and down in the image
  ASSERT_TRUE(camera.grab());
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, -0.5 * 23, 1e-6);
  EXPECT_NEAR(camera.last_truth().center_px.y - 99.5, 0.25 * 23, 1e-6);
}

TEST(SimTrayCamera, RendersTheNearestHole) {
  ManualClock clock;
  TraySight sight = sight_at(19, 20);  // 1 mm short of the third hole
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, 1.0 * 23, 1e-6);
  // the second hole, 4 mm to the left, is in view too (cut by the frame's
  // edge), dark like the nearest on the bright tray
  const auto hole = frame->at(123, 100);
  const auto neighbour = frame->at(8, 100);
  const auto tray = frame->at(60, 100);
  EXPECT_LT(hole, tray);
  EXPECT_EQ(neighbour, hole);
}

// Only the tray's own holes are drawn: no imagined grid of neighbours that a
// finder could take for one.
TEST(SimTrayCamera, DrawsTheTraysHolesAndNoOthers) {
  ManualClock clock;
  TraySight sight;
  sight.stage = {15, 20};
  sight.holes = {{15, 20}, {17, 21}};  // two holes, askew
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  vision::SimpleFinder finder;
  vision::FinderParams params;
  params.expected_radius_px = 11.5;
  const auto found = finder.find(frame->view(), params);
  ASSERT_EQ(found.size(), 2u);
  for (const auto& t : found) {
    const double dx = (t.center_px.x - 99.5) / 23, dy = -(t.center_px.y - 99.5) / 23;
    const bool first = std::hypot(dx, dy) < 0.05;
    const bool second = std::hypot(dx - 2, dy - 1) < 0.05;
    EXPECT_TRUE(first || second) << dx << ", " << dy;
  }
}

TEST(SimTrayCamera, BareTrayWithNoHoleInView) {
  ManualClock clock;
  for (TraySight sight : {sight_at(40, 40), TraySight{{15, 20}, {}, 0.5}}) {
    SimTrayCamera camera(config(), [&] { return sight; }, clock);
    auto frame = camera.grab();
    ASSERT_TRUE(frame);
    EXPECT_FALSE(camera.last_truth().visible);
    EXPECT_FALSE(seen(*frame));
  }
}

TEST(SimTrayCamera, FramesGetNewerAndNumbered) {
  ManualClock clock(TimePoint{} + 1h);
  TraySight sight = sight_at(15, 20);
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  auto a = camera.grab();
  clock.advance(40ms);
  auto b = camera.grab();
  ASSERT_TRUE(a && b);
  EXPECT_EQ(a->seq, 1u);
  EXPECT_EQ(b->seq, 2u);
  EXPECT_EQ(a->timestamp, TimePoint{} + 1h);
  EXPECT_EQ(b->timestamp - a->timestamp, Duration(40ms));
  EXPECT_EQ(camera.info().width, 200);
}

// A camera mounted the other way round gives a mirrored picture: the config
// says which way, and the simulated picture follows it.
TEST(SimTrayCamera, AFlippedConfigMirrorsTheImage) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  CameraConfig usual = config(0.2, -0.1);
  CameraConfig mirrored = usual;
  mirrored.flip_x = true;   // image +x is stage -x
  mirrored.flip_y = false;  // image +y is stage +y
  SimTrayCamera a(usual, [&] { return sight; }, clock);
  SimTrayCamera b(mirrored, [&] { return sight; }, clock);
  ASSERT_TRUE(a.grab());
  auto frame = b.grab();
  ASSERT_TRUE(frame);
  EXPECT_NEAR(a.last_truth().center_px.x - 99.5, 0.2 * 23, 1e-6);
  EXPECT_NEAR(b.last_truth().center_px.x - 99.5, -0.2 * 23, 1e-6);
  EXPECT_NEAR(b.last_truth().center_px.y - 99.5, -0.1 * 23, 1e-6);
  const auto off = seen(*frame);
  ASSERT_TRUE(off);
  EXPECT_NEAR(off->x, -0.2 * 23, 1.0);
  EXPECT_NEAR(off->y, -0.1 * 23, 1.0);
  // and the config's own map turns either picture into the same stage move
  const auto move_a = usual.map().to_mm({0.2 * 23, 0.1 * 23});
  const auto move_b = mirrored.map().to_mm({-0.2 * 23, -0.1 * 23});
  EXPECT_NEAR(move_a.x, move_b.x, 1e-9);
  EXPECT_NEAR(move_a.y, move_b.y, 1e-9);
}

TEST(SimTrayCamera, NoiseDiffersFrameToFrameAndRepeatsRunToRun) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  CameraConfig c = config();
  c.sim_noise = 0.05;
  SimTrayCamera a(c, [&] { return sight; }, clock);
  SimTrayCamera b(c, [&] { return sight; }, clock);
  auto a1 = a.grab(), a2 = a.grab(), b1 = b.grab();
  ASSERT_TRUE(a1 && a2 && b1);
  EXPECT_NE(a1->data, a2->data);
  EXPECT_EQ(a1->data, b1->data);
}

TEST(SimTrayCamera, UsesTheConfigsSizeAndScale) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  CameraConfig c = config(0.1, 0);
  c.px_per_mm = 40;
  c.sim_width = 320;
  c.sim_height = 240;
  SimTrayCamera camera(c, [&] { return sight; }, clock);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->width, 320);
  EXPECT_EQ(frame->height, 240);
  EXPECT_NEAR(camera.last_truth().center_px.x - 159.5, 4.0, 1e-6);
  EXPECT_NEAR(camera.last_truth().radius_px, 20, 1e-9);
}

TEST(MakeFrameSource, SimAndRecorded) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  auto sim = make_frame_source(config(), fs::temp_directory_path(), [&] { return sight; }, clock);
  ASSERT_TRUE(sim) << sim.error().what;
  EXPECT_TRUE((*sim)->grab());

  // a recorded case: two frames written the way fixtures are
  std::random_device rd;
  const auto lab = fs::temp_directory_path() / ("pychron_cam_" + std::to_string(rd()) + std::to_string(rd()));
  const auto dir = lab / "recordings" / "holes";
  fs::create_directories(dir);
  {
    vision::FrameRecorder recorder(dir, vision::Provenance::Synthetic, vision::FinderMode::Hole, 11.5);
    vision::HoleScene scene;
    scene.hole_mm = {0.2, 0};
    for (int i = 0; i < 2; ++i) ASSERT_TRUE(recorder.add(vision::render(scene, {0, 0}).first.view()));
    ASSERT_TRUE(recorder.finish());
  }
  CameraConfig recorded = config();
  recorded.source = CameraSource::Recorded;
  recorded.frames = "recordings/holes";
  auto source = make_frame_source(recorded, lab, [&] { return sight; }, clock);
  ASSERT_TRUE(source) << source.error().what;
  auto first = (*source)->grab();
  ASSERT_TRUE(first) << first.error().what;
  EXPECT_NEAR(seen(*first)->x, 0.2 * 23, 1.0);
  EXPECT_TRUE((*source)->grab());
  EXPECT_FALSE((*source)->grab());  // the recording ends

  recorded.frames = "recordings/no-such";
  auto missing = make_frame_source(recorded, lab, [&] { return sight; }, clock);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);
  EXPECT_NE(missing.error().what.find("no-such"), std::string::npos) << missing.error().what;
  fs::remove_all(lab);
}
