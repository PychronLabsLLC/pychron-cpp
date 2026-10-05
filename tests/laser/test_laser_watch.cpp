// The laser system watched and stopped from outside the thread that drives
// it (laser window design, section 3): the snapshot, the camera's view, and
// the emergency stop with its latch.

#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "laser_harness.hpp"

using namespace pychron;
using namespace pychron::laser;
using namespace pychron::laser::harness;

namespace {

struct LaserWatch : ::testing::Test, CameraHarness {
  IPatternRunner& runner() { return *system.pattern_runner(); }
  ILaserDevice& beam() { return *system.laser(); }
  void go(std::string_view hole, bool autocenter = false) {
    ASSERT_TRUE(system.move_to_position(hole, autocenter));
    auto ended = drive();
    ASSERT_TRUE(ended) << ended.error().what;
  }
  void finish_pattern() {
    for (int i = 0; i < 5000; ++i) {
      auto running = runner().running();
      ASSERT_TRUE(running) << running.error().what;
      if (!*running) return;
      advance();
    }
    FAIL() << "pattern still running";
  }
  bool logged(std::string_view command) const {
    const auto log = sim.log();
    return std::find(log.begin(), log.end(), command) != log.end();
  }
};

}  // namespace

TEST_F(LaserWatch, SnapshotSaysWhereAndWhat) {
  go("3");
  ASSERT_TRUE(system.enable());
  ASSERT_TRUE(system.extract(12, ExtractUnits::Percent));
  const LaserSnapshot s = system.snapshot();
  EXPECT_EQ(s.device, "co2");
  EXPECT_EQ(s.tray, "small");
  EXPECT_EQ(s.calibration, CalibrationState::Ok);
  EXPECT_TRUE(s.has_stage);
  EXPECT_TRUE(s.has_laser);
  EXPECT_TRUE(s.has_camera);
  ASSERT_TRUE(s.position);
  EXPECT_NEAR(s.position->x, 15, 1e-6);
  EXPECT_NEAR(s.position->y, 20, 1e-6);
  EXPECT_EQ(s.enabled, true);
  EXPECT_EQ(s.firing, false);
  ASSERT_TRUE(s.output);
  EXPECT_NEAR(*s.output, 12, 1e-6);
  EXPECT_TRUE(s.interlocks.empty());
  EXPECT_EQ(s.activity, LaserActivity::Idle);
  EXPECT_EQ(s.last_hole, "3");
  EXPECT_FALSE(s.stopped);
  EXPECT_TRUE(s.error.empty()) << s.error;
}

TEST_F(LaserWatch, SnapshotWithNoTraySaysSo) {
  ASSERT_TRUE(system.set_tray(""));
  const LaserSnapshot s = system.snapshot();
  EXPECT_TRUE(s.tray.empty());
  EXPECT_EQ(s.calibration, CalibrationState::Missing);
  EXPECT_TRUE(s.last_hole.empty());
}

TEST_F(LaserWatch, SnapshotListsTrippedInterlocks) {
  sim.trip_interlock("Door");
  EXPECT_EQ(system.snapshot().interlocks, (std::vector<std::string>{"Door"}));
}

TEST_F(LaserWatch, TheHoleIsForgottenOnceTheStageIsSentElsewhere) {
  go("3");
  ASSERT_TRUE(system.set_xy(12, 21));
  EXPECT_TRUE(system.snapshot().last_hole.empty());
}

TEST_F(LaserWatch, ActivityFollowsMoveCentringAndPattern) {
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Idle);
  ASSERT_TRUE(system.move_to_position("3", true));
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Centring);
  ASSERT_TRUE(drive());
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Idle);

  ASSERT_TRUE(system.set_xy(12, 21));
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Moving);
  ASSERT_TRUE(drive());
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Idle);

  ASSERT_TRUE(runner().execute_pattern("square"));
  const LaserSnapshot s = system.snapshot();
  EXPECT_EQ(s.activity, LaserActivity::Pattern);
  EXPECT_NE(s.pattern_progress.find("square"), std::string::npos) << s.pattern_progress;
  finish_pattern();
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Idle);
  EXPECT_TRUE(system.snapshot().pattern_progress.empty());
}

// A stage that fails while it is asked whether it has arrived is not shown
// as moving for ever after.
TEST_F(LaserWatch, AMoveWhoseStageFailsIsNotLeftMoving) {
  ASSERT_TRUE(system.set_xy(12, 21));
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Moving);
  sim.fail_next("Stage.Pos?", 4);
  ASSERT_FALSE(system.moving());
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Idle);
}

TEST_F(LaserWatch, TheStopCanBeLatchedWithoutWaitingForTheDevice) {
  system.latch_stop();
  EXPECT_TRUE(system.stopped());
  EXPECT_FALSE(system.enable());
  EXPECT_TRUE(sim.log().empty() || sim.log().back() != "Laser.Enable 1");
}

TEST_F(LaserWatch, SnapshotNeverAdvancesACentring) {
  ASSERT_TRUE(system.move_to_position("3", true));
  // the stage arrives and settles, but nobody polls moving()
  for (int i = 0; i < 100; ++i) advance();
  const auto before = sim.log().size();
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(system.snapshot().activity, LaserActivity::Centring);
    advance();
  }
  for (std::size_t i = before; i < sim.log().size(); ++i) {
    EXPECT_FALSE(sim.log()[i].starts_with("Stage.MoveTo")) << "a watcher moved the stage: " << sim.log()[i];
  }
  EXPECT_EQ(system.last_autocenter().result, AutocenterOutcome::Result::None);
  auto ended = drive();
  ASSERT_TRUE(ended);
  EXPECT_EQ(system.last_autocenter().result, AutocenterOutcome::Result::Converged);
}

TEST_F(LaserWatch, ViewShowsTheHoleNearTheAim) {
  go("3");
  auto seen = system.view();
  ASSERT_TRUE(seen) << seen.error().what;
  EXPECT_EQ(seen->frame.width, camera.sim_width);
  EXPECT_DOUBLE_EQ(seen->px_per_mm, camera.px_per_mm);
  EXPECT_NEAR(seen->expected_radius_px, 0.5 * camera.px_per_mm, 1e-9);
  EXPECT_NEAR(seen->aim_px.x, (camera.sim_width - 1) / 2.0, 1e-9);
  ASSERT_TRUE(seen->target);
  // the tray is 0.15, -0.10 mm from where the calibration says: a few pixels
  const double off = std::hypot(seen->target->center_px.x - seen->aim_px.x, seen->target->center_px.y - seen->aim_px.y);
  EXPECT_GT(off, 1.0);
  EXPECT_LT(off, 0.3 * camera.px_per_mm);
  // looking moves nothing
  EXPECT_NEAR(at().x, 15, 1e-6);
}

TEST_F(LaserWatch, ViewSeesNoTargetOffTheTray) {
  ASSERT_TRUE(system.set_xy(-30, -30));  // nowhere near a hole
  ASSERT_TRUE(drive());
  auto seen = system.view();
  ASSERT_TRUE(seen) << seen.error().what;
  EXPECT_FALSE(seen->target);
}

TEST(LaserWatchNoCamera, ViewNeedsACamera) {
  LaserHarness h;
  auto seen = h.system.view();
  ASSERT_FALSE(seen);
  EXPECT_EQ(seen.error().kind, ErrorKind::Config);
  EXPECT_FALSE(h.system.snapshot().has_camera);
}

TEST_F(LaserWatch, EmergencyStopEndsBeamStageAndPattern) {
  go("1");
  ASSERT_TRUE(system.enable());
  ASSERT_TRUE(system.extract(20, ExtractUnits::Percent));
  ASSERT_TRUE(beam().fire_laser());
  ASSERT_TRUE(sim.firing());
  ASSERT_TRUE(runner().execute_pattern("square"));
  advance();
  ASSERT_TRUE(*runner().running());

  auto stopped = system.emergency_stop();
  ASSERT_TRUE(stopped) << stopped.error().what;
  EXPECT_FALSE(sim.firing());
  EXPECT_FALSE(sim.enabled());
  EXPECT_DOUBLE_EQ(sim.output(), 0);
  EXPECT_TRUE(logged("Stage.Stop"));
  EXPECT_FALSE(*runner().running());
  const auto where = sim.position();
  for (int i = 0; i < 20; ++i) advance();
  EXPECT_EQ(sim.position().x, where.x) << "the stage went on moving";
  EXPECT_TRUE(system.stopped());
  const LaserSnapshot s = system.snapshot();
  EXPECT_TRUE(s.stopped);
  EXPECT_EQ(s.activity, LaserActivity::Idle);
}

TEST_F(LaserWatch, EmergencyStopAbandonsACentring) {
  ASSERT_TRUE(system.move_to_position("3", true));
  advance();
  ASSERT_TRUE(system.emergency_stop());
  EXPECT_EQ(system.last_autocenter().result, AutocenterOutcome::Result::Stopped);
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Idle);
}

TEST_F(LaserWatch, EmergencyStopTriesEveryStep) {
  ASSERT_TRUE(system.enable());
  ASSERT_TRUE(system.extract(20, ExtractUnits::Percent));
  ASSERT_TRUE(beam().fire_laser());
  ASSERT_TRUE(system.set_xy(30, 30));
  sim.fail_next("Laser.Stop", 4);  // the first step is refused
  const auto stopped = system.emergency_stop();
  EXPECT_FALSE(stopped) << "what could not be done is said";
  EXPECT_TRUE(logged("Stage.Stop")) << "and the rest is done all the same";
  EXPECT_DOUBLE_EQ(sim.output(), 0);
  EXPECT_FALSE(sim.enabled());
  EXPECT_TRUE(system.stopped());
}

TEST_F(LaserWatch, LatchedRefusesEnableFireMoveAndPattern) {
  ASSERT_TRUE(system.emergency_stop());
  const auto refused = [](const Result<void>& r) {
    return !r && r.error().kind == ErrorKind::Interlock && r.error().what.find("emergency stop") != std::string::npos;
  };
  const auto before = sim.log().size();
  EXPECT_TRUE(refused(system.enable()));
  EXPECT_TRUE(refused(system.extract(5, ExtractUnits::Percent)));
  EXPECT_TRUE(refused(beam().fire_laser()));
  EXPECT_TRUE(refused(beam().warmup()));
  EXPECT_TRUE(refused(system.move_to_position("3", false)));
  EXPECT_TRUE(refused(system.set_xy(1, 1)));
  EXPECT_TRUE(refused(system.set_axis(IStage::Axis::Z, 1)));
  EXPECT_TRUE(refused(runner().execute_pattern("square")));
  EXPECT_TRUE(refused(runner().execute_pattern_for("track", 5)));
  EXPECT_EQ(sim.log().size(), before) << "nothing was sent";
  // what makes things safer is never refused
  EXPECT_TRUE(system.end_extract());
  EXPECT_TRUE(system.disable());
  EXPECT_TRUE(system.stop());
  EXPECT_TRUE(beam().stop_laser());
  EXPECT_TRUE(runner().stop_pattern());
}

TEST_F(LaserWatch, ResetAllowsAgain) {
  ASSERT_TRUE(system.emergency_stop());
  system.reset_stop();
  EXPECT_FALSE(system.stopped());
  EXPECT_TRUE(system.enable());
  go("3");
  EXPECT_NEAR(at().x, 15, 1e-6);
}

// One thread drives (a centring, then a pattern, with the beam on); another
// watches as fast as it can. Run under the sanitizers in CI.
TEST_F(LaserWatch, WatchedWhileDriven) {
  std::atomic<bool> done{false};
  std::atomic<int> looks{0};
  std::string watcher_error;
  std::thread watcher([&] {
    while (!done.load()) {
      const LaserSnapshot s = system.snapshot();
      if (!s.error.empty() && watcher_error.empty()) watcher_error = s.error;
      auto seen = system.view();
      if (!seen && watcher_error.empty()) watcher_error = seen.error().what;
      (void)system.tray();
      ++looks;
    }
  });
  go("3", true);
  const AutocenterOutcome outcome = system.last_autocenter();
  ASSERT_TRUE(system.enable());
  ASSERT_TRUE(system.extract(20, ExtractUnits::Percent));
  ASSERT_TRUE(beam().fire_laser());
  ASSERT_TRUE(runner().execute_pattern("square"));
  finish_pattern();
  ASSERT_TRUE(system.end_extract());
  while (looks.load() < 50) std::this_thread::yield();
  done = true;
  watcher.join();
  EXPECT_TRUE(watcher_error.empty()) << watcher_error;
  EXPECT_EQ(outcome.result, AutocenterOutcome::Result::Converged);
  EXPECT_NEAR(outcome.found.x, 15.15, 0.04);
  EXPECT_NEAR(outcome.found.y, 19.90, 0.04);
}

// The stop comes from a third thread while the driver is mid-pattern.
TEST_F(LaserWatch, StoppedFromAnotherThreadMidPattern) {
  go("1");
  ASSERT_TRUE(system.enable());
  ASSERT_TRUE(system.extract(20, ExtractUnits::Percent));
  ASSERT_TRUE(beam().fire_laser());
  ASSERT_TRUE(runner().execute_pattern("twice"));
  std::thread stopper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(system.emergency_stop());
  });
  // the driver goes on polling, as a script would, until the pattern is over
  Result<bool> running = true;
  for (int i = 0; i < 200000 && running && *running; ++i) {
    running = runner().running();
    advance();
    std::this_thread::yield();
  }
  stopper.join();
  EXPECT_FALSE(sim.firing());
  EXPECT_TRUE(system.stopped());
  // whether it saw the refusal or the stop, the pattern is over
  if (running) EXPECT_FALSE(*running);
  EXPECT_FALSE(*runner().running());
}

// A camera for looking only (live camera design, section 3): its picture is
// shown and nothing it sees moves the stage.

namespace {

struct Viewer : ::testing::Test, LaserHarness {
  CameraConfig camera = [] {
    CameraConfig c = camera_config();
    c.use = CameraUse::View;
    return c;
  }();
  Viewer() {
    EXPECT_TRUE(system.attach_viewer(camera, std::make_unique<SimTrayCamera>(camera, system.sight(), clock), clock));
    EXPECT_TRUE(system.set_tray("small"));
  }
};

// A camera a test controls, behind the "pylon" backend's name.
struct FakeLive {
  struct Control {
    std::atomic<bool> fails{false};
    std::atomic<int> grabs{0};
  };
  std::shared_ptr<Control> control = std::make_shared<Control>();
  FakeLive() {
    vision::CameraBackend b;
    b.name = "pylon";
    b.available = true;
    b.open = [control = control](const vision::CameraRequest& r, vision::ClockFn stamp)
        -> Result<std::unique_ptr<vision::IFrameSource>> {
      if (r.device == "absent") return fail(ErrorKind::Io, "no camera with serial absent");
      struct Source final : vision::IFrameSource {
        std::shared_ptr<Control> control;
        vision::ClockFn stamp;
        std::uint64_t seq = 0;
        Result<vision::Frame> grab() override {
          std::this_thread::sleep_for(std::chrono::milliseconds(3));
          if (control->fails) return fail(ErrorKind::Io, "cable out");
          vision::Frame f = vision::Frame::make(64, 48, 255, 180);
          f.seq = ++seq;
          f.timestamp = stamp();
          ++control->grabs;
          return f;
        }
        vision::FrameInfo info() const override { return {64, 48, 255, 300.0}; }
      };
      auto source = std::make_unique<Source>();
      source->control = control;
      source->stamp = std::move(stamp);
      return std::unique_ptr<vision::IFrameSource>(std::move(source));
    };
    b.list = [] { return std::vector<vision::CameraFound>{}; };
    vision::register_camera_backend(std::move(b));
  }
  ~FakeLive() { vision::unregister_camera_backend("pylon"); }

  static CameraConfig config(const char* serial = "1") {
    CameraConfig c;
    c.device = "co2";
    c.source = CameraSource::Pylon;
    c.use = CameraUse::View;
    c.live_device = serial;
    c.live_timeout = std::chrono::milliseconds(200);
    return c;
  }
};

}  // namespace

TEST_F(Viewer, ShowsItsPictureAndNeverCentres) {
  EXPECT_TRUE(system.has_camera());
  EXPECT_FALSE(system.can_centre());
  EXPECT_FALSE(system.autocenter_needs_polling());
  ASSERT_TRUE(system.move_to_position("3", true));
  EXPECT_EQ(system.snapshot().activity, LaserActivity::Moving) << "a move, not a centring";
  ASSERT_TRUE(conformance::settles(*this, [&] { return system.moving(); }));
  EXPECT_EQ(sim.position().x, 15000) << "at its calibrated position, not where the camera sees it";
  EXPECT_EQ(sim.position().y, 20000);
  EXPECT_EQ(system.last_autocenter().result, AutocenterOutcome::Result::None);
  const auto seen = system.view();
  ASSERT_TRUE(seen) << seen.error().what;
  EXPECT_TRUE(seen->target.has_value()) << "what it sees is still shown";
  EXPECT_TRUE(system.snapshot().has_camera);
}

TEST_F(Viewer, ADragonflyIsRefused) {
  ASSERT_TRUE(system.move_to_position("3", false));
  ASSERT_TRUE(conformance::settles(*this, [&] { return system.moving(); }));
  const auto started = system.pattern_runner()->execute_pattern_for("track", 5);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Config);
}

TEST(ViewerRules, ACameraThatCentresIsNotAttachedAsOneThatCannot) {
  LaserHarness h;
  CameraConfig recorded = camera_config();
  recorded.source = CameraSource::Recorded;
  // a camera meant to centre must come in by attach_camera, where it is checked
  CameraConfig view = camera_config();
  view.use = CameraUse::View;
  const auto as_camera = h.system.attach_camera(view, std::make_unique<SimTrayCamera>(view, h.system.sight(), h.clock), h.clock);
  ASSERT_FALSE(as_camera);
  EXPECT_NE(as_camera.error().what.find("looking"), std::string::npos) << as_camera.error().what;
  EXPECT_FALSE(h.system.has_camera());
}

TEST(LiveView, ALiveCameraIsWatchedWithoutWaitingForIt) {
  FakeLive fake;
  LaserHarness h;
  const CameraConfig config = FakeLive::config();
  auto frames = make_frame_source(config, h.lab.dir, h.system.sight(), h.clock);
  ASSERT_TRUE(frames) << frames.error().what;
  ASSERT_TRUE(h.system.attach_viewer(config, std::move(*frames), h.clock));
  // frames arrive by themselves
  Result<CameraView> seen = h.system.view();
  for (int i = 0; i < 400 && (!seen || seen->fps <= 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    seen = h.system.view();
  }
  ASSERT_TRUE(seen) << seen.error().what;
  EXPECT_EQ(seen->frame.width, 64);
  EXPECT_GT(seen->fps, 0);
  EXPECT_LT(seen->age_ms, 200);
  EXPECT_TRUE(seen->trouble.empty()) << seen->trouble;
  // the camera goes: the last picture, how old, and why; and the look does not wait for it
  fake.control->fails = true;
  for (int i = 0; i < 400 && seen && seen->trouble.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    seen = h.system.view();
  }
  ASSERT_TRUE(seen);
  EXPECT_NE(seen->trouble.find("cable out"), std::string::npos) << seen->trouble;
  EXPECT_EQ(seen->frame.width, 64);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto began = std::chrono::steady_clock::now();
  seen = h.system.view();
  EXPECT_LT(std::chrono::steady_clock::now() - began, std::chrono::milliseconds(40));
  ASSERT_TRUE(seen);
  EXPECT_GE(seen->age_ms, 50);
  // and it comes back by itself
  fake.control->fails = false;
  for (int i = 0; i < 600 && (!seen || !seen->trouble.empty()); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    seen = h.system.view();
  }
  ASSERT_TRUE(seen);
  EXPECT_TRUE(seen->trouble.empty()) << seen->trouble;
}

TEST(LiveView, ACameraThatIsNotThereIsStillWaitedFor) {
  FakeLive fake;
  LaserHarness h;
  // absent now: the source is made all the same, and says what is wrong
  auto frames = make_frame_source(FakeLive::config("absent"), h.lab.dir, h.system.sight(), h.clock);
  ASSERT_TRUE(frames) << frames.error().what;
  ASSERT_TRUE(h.system.attach_viewer(FakeLive::config("absent"), std::move(*frames), h.clock));
  const auto seen = h.system.view();
  ASSERT_FALSE(seen);
  EXPECT_NE(seen.error().what.find("no camera with serial absent"), std::string::npos) << seen.error().what;
}

TEST(LiveView, ABackendThisBuildLacksIsAnError) {
  LaserHarness h;  // the real "pylon": not built
  const auto frames = make_frame_source(FakeLive::config(), h.lab.dir, h.system.sight(), h.clock);
  ASSERT_FALSE(frames);
  EXPECT_EQ(frames.error().kind, ErrorKind::Config);
  EXPECT_NE(frames.error().what.find("built without pylon"), std::string::npos) << frames.error().what;
  EXPECT_NE(frames.error().what.find("co2"), std::string::npos) << frames.error().what;
}
