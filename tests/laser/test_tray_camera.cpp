// The simulated camera: the tray as a camera looking down the beam path
// would see it (laser autocenter design, section 4).

#include "pychron/laser/tray_camera.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <numbers>
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

// Where the finder sees a hole, as an offset in px from the image center.
std::optional<vision::Vec2> seen(const vision::Frame& frame, double px_per_mm = 23.0) {
  vision::SimpleFinder finder;
  vision::FinderParams params;
  params.mode = vision::FinderMode::Hole;
  params.expected_radius_px = 0.5 * px_per_mm;
  // the hole nearest the center
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

// While the laser fires the camera does not see a dark hole: it sees the
// sample glow, where the sample is.
namespace {
struct Glow {
  double x_px = 0, y_px = 0, saturation = 0;
};
std::optional<Glow> glow(const vision::Frame& frame) {
  vision::SimpleFinder finder;
  vision::FinderParams params;
  params.mode = vision::FinderMode::Glow;
  params.expected_radius_px = 0.5 * 23;
  const auto found = finder.find(frame.view(), params);
  if (found.empty()) return std::nullopt;
  return Glow{found.front().center_px.x - (frame.width - 1) / 2.0, found.front().center_px.y - (frame.height - 1) / 2.0,
              vision::saturation(found.front())};
}
}  // namespace

TEST(SimTrayCamera, ShowsAGlowAtTheGrainWhileFiring) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  CameraConfig c = config(0.1, 0);       // the tray, 0.1 mm right
  c.sim_grain_offset_mm = {0.2, 0.15};   // and the grain off its hole's center
  SimTrayCamera camera(c, [&] { return sight; }, clock);

  auto dark = camera.grab();
  ASSERT_TRUE(dark);
  EXPECT_TRUE(seen(*dark));     // not firing: the hole
  EXPECT_FALSE(glow(*dark));

  sight.firing = true;
  sight.output_percent = 20;
  auto lit = camera.grab();
  ASSERT_TRUE(lit);
  const auto g = glow(*lit);
  ASSERT_TRUE(g);
  EXPECT_NEAR(g->x_px, 0.3 * 23, 1.0);    // 0.1 + 0.2, right
  EXPECT_NEAR(g->y_px, -0.15 * 23, 1.0);  // 0.15 up: image y is down
  EXPECT_TRUE(camera.last_truth().visible);
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, 0.3 * 23, 1e-6);
  EXPECT_FALSE(seen(*lit));     // no dark hole to take for one while it glows

  sight.firing = false;
  auto after = camera.grab();
  ASSERT_TRUE(after);
  EXPECT_FALSE(glow(*after));
}

TEST(SimTrayCamera, TheGlowDriftsWhileFiring) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  CameraConfig c = config();
  c.sim_glow_drift_mm_per_s = {0.02, -0.01};
  SimTrayCamera camera(c, [&] { return sight; }, clock);
  sight.firing = true;
  sight.output_percent = 20;
  ASSERT_TRUE(camera.grab());
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, 0, 1e-6);
  clock.advance(10s);
  ASSERT_TRUE(camera.grab());
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, 0.2 * 23, 1e-6);
  EXPECT_NEAR(camera.last_truth().center_px.y - 99.5, 0.1 * 23, 1e-6);  // -0.1 mm in y: down the image is +
  // the beam goes off and on again: the grain is where it had got to, and creeps on from there
  sight.firing = false;
  clock.advance(100s);
  ASSERT_TRUE(camera.grab());
  sight.firing = true;
  ASSERT_TRUE(camera.grab());
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, 0.2 * 23, 1e-6);
  clock.advance(5s);
  ASSERT_TRUE(camera.grab());
  EXPECT_NEAR(camera.last_truth().center_px.x - 99.5, 0.3 * 23, 1e-6);
}

TEST(SimTrayCamera, BrightnessFollowsOutput) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  sight.firing = true;
  const auto saturation_at = [&](double percent) {
    sight.output_percent = percent;
    auto frame = camera.grab();
    EXPECT_TRUE(frame);
    const auto g = glow(*frame);
    return g ? g->saturation : 0.0;
  };
  const double low = saturation_at(8), mid = saturation_at(20), full = saturation_at(100);
  EXPECT_LT(low, mid);
  EXPECT_LT(mid, 0.75);   // at a working output the glow is not saturated: it can be followed
  EXPECT_GE(full, 0.75);  // flat out, it is
  sight.output_percent = 0;
  auto off = camera.grab();
  ASSERT_TRUE(off);
  EXPECT_FALSE(glow(*off));  // firing at no output: nothing glows
}

TEST(SimTrayCamera, NoGlowWithNoHoleInView) {
  ManualClock clock;
  TraySight sight = sight_at(40, 40);
  sight.firing = true;
  sight.output_percent = 20;
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  EXPECT_FALSE(glow(*frame));
  EXPECT_FALSE(camera.last_truth().visible);
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

namespace {
// FNV-1a over the pixels of a frame, folded into a running hash.
std::uint64_t fold(std::uint64_t h, const vision::Frame& frame) {
  for (const auto v : frame.data) {
    h ^= v;
    h *= 1099511628211ULL;
  }
  return h;
}
}  // namespace

// Without a [sim.grain] table the camera draws what it drew before the grain
// model: the hash was recorded on the code before that change, with noise on
// so the noise path is held too.
TEST(SimTrayCamera, NoGrainTableIsTodaysPicture) {
  ManualClock clock;
  TraySight sight = sight_at(15, 20);
  CameraConfig c = config(0.1, -0.05);
  c.sim_noise = 0.02;
  c.sim_grain_offset_mm = {0.2, 0.1};
  c.sim_glow_drift_mm_per_s = {0.02, -0.01};
  SimTrayCamera camera(c, [&] { return sight; }, clock);
  std::uint64_t h = 14695981039346656037ULL;
  const auto take = [&] {
    auto frame = camera.grab();
    ASSERT_TRUE(frame);
    h = fold(h, *frame);
  };
  take();  // the tray
  sight.firing = true;
  sight.output_percent = 20;
  take();
  clock.advance(3s);
  take();
  sight.stage = {15.1, 20.05};
  clock.advance(2s);
  take();
  sight.firing = false;
  take();
  // On a failure on one platform only, diff the frames (a boundary pixel,
  // say) to find what differs; never record the hash again to get past it.
  EXPECT_EQ(h, 9287573347953853465ULL);
}

// The simulated grain (the dragonfly grain simulator design, section 5).
namespace {

CameraConfig grain_config(const vision::GrainParams& grain, double gx = 0, double gy = 0) {
  CameraConfig c = config();
  c.sim_grain = grain;
  c.sim_grain_offset_mm = {gx, gy};
  return c;
}

TraySight firing_at(double x, double y, double percent = 20) {
  TraySight s = sight_at(x, y);
  s.firing = true;
  s.output_percent = percent;
  return s;
}

// Grabs every 100 ms for `seconds` of the clock.
void run(SimTrayCamera& camera, ManualClock& clock, double seconds) {
  for (int i = 0; i < static_cast<int>(seconds * 10); ++i) {
    ASSERT_TRUE(camera.grab());
    clock.advance(100ms);
  }
}

std::uint64_t hash_of(const vision::Frame& frame) { return fold(14695981039346656037ULL, frame); }

}  // namespace

TEST(SimTrayCamera, GrainGlowsWhereTheModelSays) {
  ManualClock clock;
  TraySight sight = firing_at(15, 20);  // the beam on the hole's center
  SimTrayCamera camera(grain_config({}, 0.2, 0.1), [&] { return sight; }, clock);
  run(camera, clock, 5);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  const auto view = camera.observe();
  ASSERT_TRUE(view);
  EXPECT_NEAR(view->grain_mm.x, 15.2, 1e-9);
  EXPECT_NEAR(view->grain_mm.y, 20.1, 1e-9);
  EXPECT_GT(view->truth.heat, 0.8);
  int bx = 0, by = 0;
  std::uint16_t best = 0;
  for (int y = 0; y < frame->height; ++y) {
    for (int x = 0; x < frame->width; ++x) {
      if (frame->at(x, y) > best) {
        best = frame->at(x, y);
        bx = x;
        by = y;
      }
    }
  }
  EXPECT_NEAR(bx, 99.5 + 0.2 * 23, 1.0);
  EXPECT_NEAR(by, 99.5 - 0.1 * 23, 1.0);  // image y is down
}

TEST(SimTrayCamera, ASizedGrainGlowsOverItsOutline) {
  vision::GrainParams p;
  p.grain_radius_mm = 0.5;
  ManualClock clock;
  TraySight sight = firing_at(15, 20);
  SimTrayCamera camera(grain_config(p), [&] { return sight; }, clock);
  run(camera, clock, 5);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  // Lit is anything well above the background, dark 0.02: the body's own level or more.
  const auto bright = std::count_if(frame->data.begin(), frame->data.end(), [](auto v) { return v > 0.15 * 255; });
  const double mm2 = static_cast<double>(bright) / (23.0 * 23.0);
  const auto view = camera.observe();
  ASSERT_TRUE(view);
  const double grain = std::numbers::pi * view->truth.radius_mm * view->truth.radius_mm;
  EXPECT_NEAR(mm2, grain, 0.25 * grain);
  EXPECT_NEAR(view->truth.radius_mm, 0.5, 1e-9);
}

TEST(SimTrayCamera, ASizedGrainIsDarkWithNoHoleInView) {
  vision::GrainParams p;
  p.grain_radius_mm = 0.5;
  ManualClock clock;
  TraySight sight = firing_at(100, 100);  // far from every hole
  SimTrayCamera camera(grain_config(p), [&] { return sight; }, clock);
  run(camera, clock, 2);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  EXPECT_LT(*std::max_element(frame->data.begin(), frame->data.end()), 0.05 * 255);
  EXPECT_FALSE(camera.last_truth().visible);
}

TEST(SimTrayCamera, BeamParkedOffTheGrainGoesDark) {
  ManualClock clock;
  TraySight sight = firing_at(17, 20);  // 2 mm from the hole, which is still in view
  SimTrayCamera camera(grain_config({}, 0.2, 0.1), [&] { return sight; }, clock);
  run(camera, clock, 10);
  auto frame = camera.grab();
  ASSERT_TRUE(frame);
  const auto top = *std::max_element(frame->data.begin(), frame->data.end());
  EXPECT_LT(top, 0.05 * 255);  // the background is 0.02
  const auto view = camera.observe();
  ASSERT_TRUE(view);
  EXPECT_LT(view->truth.heat, 1e-3);
}

TEST(SimTrayCamera, ObserveMovesTheModelWithoutDrawing) {
  ManualClock clock;
  TraySight sight = firing_at(15, 20);
  SimTrayCamera camera(grain_config({}), [&] { return sight; }, clock);
  auto first = camera.grab();
  ASSERT_TRUE(first);
  EXPECT_EQ(first->seq, 1u);
  clock.advance(500ms);
  const auto early = camera.observe();
  ASSERT_TRUE(early);
  clock.advance(500ms);
  const auto later = camera.observe();
  ASSERT_TRUE(later);
  EXPECT_GT(early->truth.heat, 0);
  EXPECT_GT(later->truth.heat, early->truth.heat);
  EXPECT_NEAR(later->beam_mm.x, 15, 1e-12);
  auto second = camera.grab();
  ASSERT_TRUE(second);
  EXPECT_EQ(second->seq, 2u);  // the looks drew nothing
}

TEST(SimTrayCamera, ObserveWithoutAGrainTableIsEmpty) {
  ManualClock clock;
  TraySight sight = firing_at(15, 20);
  SimTrayCamera camera(config(), [&] { return sight; }, clock);
  EXPECT_FALSE(camera.observe());
  TraySight bare;  // no tray, no holes
  SimTrayCamera none(grain_config({}), [&] { return bare; }, clock);
  EXPECT_FALSE(none.observe());
}

TEST(SimTrayCamera, ObserveThenGrabEqualsGrab) {
  vision::GrainParams p;
  p.flicker = 0.3;
  p.flares_per_s = 2;
  p.lobes = 3;
  p.lobe_spread_mm = 0.2;
  p.lobe_wander_mm_per_rt_s = 0.2;
  p.wander_mm_per_rt_s = 0.1;
  p.jumps_per_s = 0.5;
  p.dropouts_per_s = 0.3;
  ManualClock clock_a, clock_b;
  TraySight sight_a = firing_at(15, 20), sight_b = firing_at(15, 20);
  CameraConfig c = grain_config(p, 0.1, 0);
  c.sim_noise = 0.02;
  SimTrayCamera a(c, [&] { return sight_a; }, clock_a);
  SimTrayCamera b(c, [&] { return sight_b; }, clock_b);
  for (int i = 0; i < 30; ++i) {
    auto fa = a.grab();
    auto fb = b.grab();
    ASSERT_TRUE(fa);
    ASSERT_TRUE(fb);
    EXPECT_EQ(hash_of(*fa), hash_of(*fb)) << "grab " << i;
    clock_a.advance(130ms);
    clock_b.advance(70ms);
    ASSERT_TRUE(b.observe());  // b looks in between; the beam does not move
    clock_b.advance(60ms);
  }
}

TEST(SimTrayCamera, ANewHoleIsANewGrain) {
  vision::GrainParams p;
  p.wander_mm_per_rt_s = 1;
  p.jumps_per_s = 2;
  ManualClock clock;
  TraySight sight = firing_at(10, 20);  // the first hole
  SimTrayCamera camera(grain_config(p, 0.1, 0.05), [&] { return sight; }, clock);
  ASSERT_TRUE(camera.observe());
  clock.advance(5s);
  const auto first = camera.observe();
  ASSERT_TRUE(first);
  EXPECT_GT(first->truth.heat, 0.5);
  EXPECT_GT(std::hypot(first->truth.center_mm.x - 0.1, first->truth.center_mm.y - 0.05), 1e-6);  // it moved
  sight = firing_at(20, 20);  // the third
  const auto other = camera.observe();
  ASSERT_TRUE(other);
  EXPECT_DOUBLE_EQ(other->truth.heat, 0);
  EXPECT_NEAR(other->grain_mm.x, 20.1, 1e-9);
  EXPECT_NEAR(other->grain_mm.y, 20.05, 1e-9);
  clock.advance(2s);
  sight = firing_at(10, 20);  // and back to the first: as it was left, cooler
  const auto back = camera.observe();
  ASSERT_TRUE(back);
  EXPECT_LT(back->truth.heat, first->truth.heat);
  EXPECT_GT(back->truth.heat, 0);
  EXPECT_GT(std::hypot(back->truth.center_mm.x - 0.1, back->truth.center_mm.y - 0.05), 1e-6);  // not a new one
}

namespace {

vision::GrainParams sized_grain() {
  vision::GrainParams p;
  p.grain_radius_mm = 0.25;
  p.grain_sides = 6;
  p.grain_irregularity = 0;
  return p;
}

}  // namespace

TEST(SimTrayCamera, AGrainLiesInItsHoleWithTheBeamOff) {
  ManualClock clock;
  TraySight sight = sight_at(10, 20);
  SimTrayCamera bare(config(), [&] { return sight; }, clock);
  SimTrayCamera with(grain_config(sized_grain()), [&] { return sight; }, clock);
  auto empty = bare.grab();
  auto grain = with.grab();
  ASSERT_TRUE(empty);
  ASSERT_TRUE(grain);
  const int cx = empty->width / 2, cy = empty->height / 2;
  EXPECT_GT(grain->at(cx, cy), empty->at(cx, cy) + 40);  // lighter than the empty hole
  EXPECT_EQ(grain->at(0, 0), empty->at(0, 0));           // the tray is the tray
}

TEST(SimTrayCamera, APointGrainDrawsNothing) {
  ManualClock clock;
  TraySight sight = sight_at(10, 20);
  SimTrayCamera bare(config(), [&] { return sight; }, clock);
  SimTrayCamera with(grain_config(vision::GrainParams{}), [&] { return sight; }, clock);
  auto empty = bare.grab();
  auto grain = with.grab();
  ASSERT_TRUE(empty);
  ASSERT_TRUE(grain);
  EXPECT_EQ(empty->at(empty->width / 2, empty->height / 2), grain->at(grain->width / 2, grain->height / 2));
}

TEST(SimTrayCamera, AMeltedGrainStaysMeltedWhenTheStageComesBack) {
  auto p = sized_grain();
  p.melt_mm_per_s = 0.05;
  p.min_radius_mm = 0.05;
  ManualClock clock;
  TraySight sight = firing_at(10, 20);
  SimTrayCamera camera(grain_config(p), [&] { return sight; }, clock);
  ASSERT_TRUE(camera.observe());
  clock.advance(3s);
  const auto melted = camera.observe();
  ASSERT_TRUE(melted);
  EXPECT_LT(melted->truth.radius_mm, 0.25);
  sight = firing_at(20, 20);
  ASSERT_TRUE(camera.observe());
  sight = sight_at(10, 20);
  const auto back = camera.observe();
  ASSERT_TRUE(back);
  EXPECT_DOUBLE_EQ(back->truth.radius_mm, melted->truth.radius_mm);
}

TEST(SimTrayCamera, ALongAbsenceCostsNothingAndMovesNothing) {
  vision::GrainParams p = sized_grain();
  p.wander_mm_per_rt_s = 1;
  ManualClock clock;
  TraySight sight = sight_at(10, 20);
  SimTrayCamera camera(grain_config(p, 0.1, 0.05), [&] { return sight; }, clock);
  ASSERT_TRUE(camera.observe());
  sight = sight_at(20, 20);
  ASSERT_TRUE(camera.observe());
  clock.advance(24h);
  sight = sight_at(10, 20);
  const auto back = camera.observe();
  ASSERT_TRUE(back);
  EXPECT_NEAR(back->truth.center_mm.x, 0.1, 1e-12);
  EXPECT_NEAR(back->truth.center_mm.y, 0.05, 1e-12);
}

TEST(SimTrayCamera, AResetPutsEveryGrainBack) {
  auto p = sized_grain();
  p.melt_mm_per_s = 0.05;
  p.min_radius_mm = 0.05;
  p.creep_mm_per_s = {0.05, 0};
  ManualClock clock;
  TraySight sight = firing_at(10, 20);
  SimTrayCamera camera(grain_config(p, 0.1, 0.05), [&] { return sight; }, clock);
  const auto start = camera.observe();
  ASSERT_TRUE(start);
  clock.advance(3s);
  const auto after = camera.observe();
  ASSERT_TRUE(after);
  EXPECT_GT(after->truth.center_mm.x, start->truth.center_mm.x);
  EXPECT_LT(after->truth.radius_mm, start->truth.radius_mm);

  camera.reset_grains();
  const auto again = camera.observe();
  ASSERT_TRUE(again);
  EXPECT_DOUBLE_EQ(again->truth.center_mm.x, start->truth.center_mm.x);
  EXPECT_DOUBLE_EQ(again->truth.radius_mm, start->truth.radius_mm);
  EXPECT_DOUBLE_EQ(again->truth.heat, 0);
}

TEST(SimTrayCamera, GrainSeedsDiffer) {
  vision::GrainParams p;
  p.lobes = 3;
  p.lobe_spread_mm = 0.3;
  p.flares_per_s = 2;
  p.flicker = 0.3;
  const auto frames_of = [&](std::uint64_t seed) {
    ManualClock clock;
    TraySight sight = firing_at(15, 20);
    CameraConfig c = grain_config(p, 0.05, 0);
    c.sim_noise = 0;  // only the model can make a difference
    SimTrayCamera camera(c, [&] { return sight; }, clock);
    camera.set_grain_seed(seed);
    std::uint64_t h = 0;
    for (int i = 0; i < 20; ++i) {
      auto frame = camera.grab();
      EXPECT_TRUE(frame);
      h = h * 31 + hash_of(*frame);
      clock.advance(100ms);
    }
    return h;
  };
  EXPECT_EQ(frames_of(1), frames_of(1));
  EXPECT_EQ(frames_of(2), frames_of(2));
  EXPECT_NE(frames_of(1), frames_of(2));
}

TEST(SimTrayCamera, AimOffsetIsWhereTheBeamIs) {
  CameraConfig c = grain_config({}, 0.2, 0.1);
  c.aim_offset_px = {23, 0};  // the beam is about 1 mm from the image's center
  const vision::Vec2 aim = c.map().to_mm({23, 0});
  ASSERT_GT(std::hypot(aim.x, aim.y), 0.5);
  const auto heat_with_stage_at = [&](double x, double y) {
    ManualClock clock;
    TraySight sight = firing_at(x, y);
    SimTrayCamera camera(c, [&] { return sight; }, clock);
    run(camera, clock, 5);
    const auto view = camera.observe();
    EXPECT_TRUE(view);
    return view->truth.heat;
  };
  // the aim point is on the grain
  EXPECT_GT(heat_with_stage_at(15.2 - aim.x, 20.1 - aim.y), 0.9);
  // the stage is centered on the grain, so the aim point is about 1 mm away
  EXPECT_LT(heat_with_stage_at(15.2, 20.1), 0.2);
}
