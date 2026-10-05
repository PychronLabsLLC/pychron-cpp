// A dragonfly pattern: the stage follows the glowing sample while the laser
// heats it (laser dragonfly design). The laser system on the Chromium
// simulator; the simulated camera shows the glow where the grain is while
// the simulated laser fires.

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "laser_harness.hpp"
#include "pychron/laser/pattern_runner.hpp"

using namespace pychron;
using namespace pychron::extraction;
using namespace pychron::laser;
using namespace pychron::laser::harness;
using namespace std::chrono_literals;

namespace {

// Frames from the simulated camera that a test can blank (the glow hidden)
// or cut off (the camera dead).
class Shutter final : public vision::IFrameSource {
 public:
  explicit Shutter(std::unique_ptr<vision::IFrameSource> inner) : inner_(std::move(inner)) {}
  Result<vision::Frame> grab() override {
    ++grabs;
    if (dead) return fail(ErrorKind::Io, "the camera stopped answering");
    auto frame = inner_->grab();
    if (after_grab) after_grab(grabs);
    if (frame && blank) std::fill(frame->data.begin(), frame->data.end(), std::uint16_t{0});
    return frame;
  }
  vision::FrameInfo info() const override { return inner_->info(); }
  bool blank = false;
  bool dead = false;
  int grabs = 0;
  std::function<void(int)> after_grab;  // told the count, once the frame is taken

 private:
  std::unique_ptr<vision::IFrameSource> inner_;
};

// The grain is `grain` from hole 3's centre; the tray is where it is calibrated.
CameraConfig glow_camera(double gx, double gy, double drift_x = 0) {
  CameraConfig c = camera_config(0, 0);
  c.sim_grain_offset_mm = {gx, gy};
  c.sim_glow_drift_mm_per_s = {drift_x, 0};
  return c;
}

struct Rig : LaserHarness {
  CorrectionStore corrections{lab.dir / "stage_corrections"};
  Shutter* shutter = nullptr;

  explicit Rig(CameraConfig camera = glow_camera(0.3, -0.2), const char* hole = "3") {
    system.set_corrections(corrections);
    auto frames = std::make_unique<Shutter>(std::make_unique<SimTrayCamera>(camera, system.sight(), clock));
    shutter = frames.get();
    EXPECT_TRUE(system.attach_camera(camera, std::move(frames), clock));
    EXPECT_TRUE(system.set_tray("small"));
    // on hole 3 (15, 20), uncentred, beam on at a working output
    EXPECT_TRUE(system.move_to_position(hole, false));
    EXPECT_TRUE(conformance::settles(*this, [&] { return system.moving(); }));
    EXPECT_TRUE(system.enable());
    EXPECT_TRUE(system.extract(20, ExtractUnits::Percent));
    EXPECT_TRUE(system.laser()->fire_laser());
  }
  IPatternRunner& runner() { return *system.pattern_runner(); }
  StageXY at() const {
    const auto p = sim.position();
    return {static_cast<double>(p.x) / 1000.0, static_cast<double>(p.y) / 1000.0};
  }
  std::vector<StageXY> targets() const {  // of every stage move so far, in mm
    std::vector<StageXY> out;
    for (const auto& line : sim.log()) {
      if (!line.starts_with("Stage.MoveTo ")) continue;
      long long x = 0, y = 0;
      char comma = 0;
      std::istringstream in(line.substr(13));
      in >> x >> comma >> y;
      out.push_back({static_cast<double>(x) / 1000.0, static_cast<double>(y) / 1000.0});
    }
    return out;
  }
  // Polls running() every 100 ms of clock for `seconds`, or until it ends.
  Result<bool> run_for(double seconds) {
    Result<bool> busy = true;
    for (int i = 0; i < static_cast<int>(seconds * 10); ++i) {
      busy = runner().running();
      if (!busy || !*busy) return busy;
      clock.advance(100ms);
    }
    return busy;
  }
  Result<bool> finish() { return run_for(600); }
};

double apart(StageXY a, StageXY b) { return std::hypot(a.x - b.x, a.y - b.y); }

}  // namespace

TEST(Dragonfly, FollowsAnOffsetGrain) {
  Rig rig;
  const TimePoint started = rig.clock.now();
  const auto logged_before = rig.sim.log().size();
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  EXPECT_TRUE(*rig.runner().running());
  ASSERT_TRUE(rig.run_for(8));
  // on the grain: (15.3, 19.8), to within the deadband and a pixel
  EXPECT_NEAR(rig.at().x, 15.3, 0.06);
  EXPECT_NEAR(rig.at().y, 19.8, 0.06);
  ASSERT_TRUE(rig.run_for(8));
  EXPECT_NEAR(rig.at().x, 15.3, 0.06);  // and it stays there
  auto done = rig.finish();
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_FALSE(*done);
  // it ran for its 20 s (and the trip home), and ends where it began
  EXPECT_GE(rig.clock.now() - started, Duration(20s));
  EXPECT_LT(rig.clock.now() - started, Duration(23s));
  EXPECT_NEAR(rig.at().x, 15, 1e-9);
  EXPECT_NEAR(rig.at().y, 20, 1e-9);
  EXPECT_FALSE(*rig.system.moving());
  EXPECT_EQ(rig.runner().last_note(), "");
  // its moves were at the pattern's 2 mm/s (shared between the axes), never the stage's travel speed
  const auto log = rig.sim.log();
  int moves = 0;
  for (std::size_t i = logged_before; i < log.size(); ++i) {
    if (!log[i].starts_with("Stage.MoveTo ")) continue;
    ++moves;
    EXPECT_FALSE(log[i].ends_with(",5000,5000,100")) << log[i];
  }
  EXPECT_GE(moves, 2);
}

TEST(Dragonfly, FollowsADriftingGrain) {
  Rig rig(glow_camera(0.1, 0, 0.02));  // creeping 0.02 mm/s in x under the beam
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(3));
  for (int second = 3; second < 19; ++second) {
    ASSERT_TRUE(rig.run_for(1));
    const double grain_x = 15.1 + 0.02 * second;  // near enough: the beam has been on about that long
    EXPECT_NEAR(rig.at().x, grain_x, 0.12) << "after " << second << " s";
  }
  EXPECT_GT(rig.at().x, 15.35);  // it really went with it
}

TEST(Dragonfly, HoldsWhenTheGlowIsSaturated) {
  Rig rig;
  ASSERT_TRUE(rig.system.extract(100, ExtractUnits::Percent));  // flat out: nothing to steer by
  const auto before = rig.targets().size();
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(10));
  EXPECT_EQ(rig.targets().size(), before);
  EXPECT_NEAR(rig.at().x, 15, 1e-9);
}

TEST(Dragonfly, SearchesWhenTheGlowIsLostAndFindsItAgain) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(5));
  ASSERT_NEAR(rig.at().x, 15.3, 0.06);
  const auto tracked = rig.targets().size();
  rig.shutter->blank = true;  // smoke, say
  ASSERT_TRUE(rig.run_for(5));
  const auto searching = rig.targets();
  EXPECT_GT(searching.size(), tracked + 1);  // it went looking
  for (const auto& t : searching) EXPECT_LE(apart(t, {15, 20}), 2.5 + 0.001);
  rig.shutter->blank = false;
  ASSERT_TRUE(rig.run_for(6));
  EXPECT_NEAR(rig.at().x, 15.3, 0.06);  // and came back to it
  EXPECT_NEAR(rig.at().y, 19.8, 0.06);
}

TEST(Dragonfly, NeverLeavesItsPerimeter) {
  Rig rig(glow_camera(0.9, 0));  // the grain is 0.9 mm off; the pattern may go 0.4
  ASSERT_TRUE(rig.runner().execute_pattern("track_tight"));
  const auto before = rig.targets().size();
  ASSERT_TRUE(rig.run_for(15));
  const auto moves = rig.targets();
  ASSERT_GT(moves.size(), before);
  for (std::size_t i = before; i < moves.size(); ++i) EXPECT_LE(apart(moves[i], {15, 20}), 0.4 + 0.001) << i;
  EXPECT_LE(apart(rig.at(), {15, 20}), 0.4 + 0.011);
  EXPECT_GT(rig.at().x, 15.2);  // it went as far as it may, towards the grain
}

TEST(Dragonfly, ADurationShorterThanASettleStillEnds) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track_brief"));  // 0.05 s; the camera's settle is 0.2
  auto done = rig.finish();
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_FALSE(*done);
  EXPECT_NEAR(rig.at().x, 15, 1e-9);
  EXPECT_EQ(rig.shutter->grabs, 0);  // over before a frame could be trusted
  // and it says so: a pattern that never looked followed nothing
  const std::string note = rig.runner().last_note();
  EXPECT_NE(note.find("track_brief"), std::string::npos) << note;
  EXPECT_NE(note.find("never looked"), std::string::npos) << note;
}

TEST(Dragonfly, AClockThatJumpsPastTheEndEndsIt) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(4));
  rig.clock.advance(1h);  // the machine slept
  auto done = rig.finish();
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_NEAR(rig.at().x, 15, 1e-9);
  EXPECT_NEAR(rig.at().y, 20, 1e-9);
}

TEST(Dragonfly, StopPartWayStopsTheStage) {
  Rig rig(glow_camera(0.9, 0));
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  // until it is on its way somewhere
  for (int i = 0; i < 100 && !*rig.system.moving(); ++i) {
    ASSERT_TRUE(rig.runner().running());
    rig.clock.advance(50ms);
  }
  ASSERT_TRUE(rig.runner().stop_pattern());
  EXPECT_EQ(rig.sim.log().back(), "Stage.Pos?");
  EXPECT_FALSE(*rig.runner().running());
  EXPECT_FALSE(*rig.system.moving());
  const auto where = rig.at();
  const int grabs = rig.shutter->grabs;
  for (int i = 0; i < 50; ++i) {
    EXPECT_FALSE(*rig.runner().running());
    rig.clock.advance(100ms);
  }
  EXPECT_NEAR(rig.at().x, where.x, 1e-9);  // not home, not onward: where it was
  EXPECT_EQ(rig.shutter->grabs, grabs);
  EXPECT_TRUE(rig.runner().execute_pattern("square"));  // and another pattern can start
}

TEST(Dragonfly, OnePollOneStageCommand) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  auto sent = rig.targets().size();
  for (int i = 0; i < 400; ++i) {
    auto busy = rig.runner().running();
    ASSERT_TRUE(busy);
    ASSERT_LE(rig.targets().size(), sent + 1);
    sent = rig.targets().size();
    if (!*busy) break;
    rig.clock.advance(100ms);
  }
}

// The camera dies with the beam on. The stage goes back to where the pattern
// began; what then is the camera's on_failure.
TEST(Dragonfly, ACameraThatDiesHoldsAtTheStartUntilTheEnd) {
  Rig rig;  // on_failure = continue
  const TimePoint started = rig.clock.now();
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(5));
  ASSERT_NEAR(rig.at().x, 15.3, 0.06);
  rig.shutter->dead = true;
  ASSERT_TRUE(rig.run_for(3));
  EXPECT_NEAR(rig.at().x, 15, 1e-9);  // back at the hole
  EXPECT_NEAR(rig.at().y, 20, 1e-9);
  EXPECT_TRUE(*rig.runner().running());  // still heating, for the rest of its time
  auto done = rig.finish();
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_FALSE(*done);
  EXPECT_GE(rig.clock.now() - started, Duration(20s));
  EXPECT_LT(rig.clock.now() - started, Duration(22s));
  const std::string note = rig.runner().last_note();
  EXPECT_NE(note.find("track"), std::string::npos) << note;
  EXPECT_NE(note.find("camera"), std::string::npos) << note;
  EXPECT_EQ(rig.runner().last_note(), "");  // said once
}

TEST(Dragonfly, ACameraThatDiesIsAnErrorWhenAskedFor) {
  CameraConfig camera = glow_camera(0.3, -0.2);
  camera.on_failure = OnAutocenterFailure::Fail;
  Rig rig(camera);
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(5));
  rig.shutter->dead = true;
  auto done = rig.finish();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Config);
  EXPECT_NE(done.error().what.find("track"), std::string::npos) << done.error().what;
  EXPECT_NE(done.error().what.find("camera"), std::string::npos) << done.error().what;
  EXPECT_NEAR(rig.at().x, 15, 1e-9);  // back at the hole before it said so
  EXPECT_FALSE(*rig.runner().running());
}

// Frames that never get newer cannot be steered by.
TEST(Dragonfly, WithoutACameraItIsRefused) {
  LaserHarness h;  // no camera
  const auto before = h.sim.log().size();
  auto r = h.system.pattern_runner()->execute_pattern("track");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("camera"), std::string::npos) << r.error().what;
  EXPECT_NE(r.error().what.find("track"), std::string::npos) << r.error().what;
  EXPECT_EQ(h.sim.log().size(), before);
  EXPECT_FALSE(*h.system.pattern_runner()->running());
  EXPECT_TRUE(h.system.pattern_runner()->execute_pattern("square"));  // paths need no camera
}

TEST(Dragonfly, IsNotStartedWhileTheStageMoves) {
  Rig rig;
  ASSERT_TRUE(rig.system.set_xy(10, 20));
  auto r = rig.runner().execute_pattern("track");
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("moving"), std::string::npos) << r.error().what;
}

TEST(Dragonfly, ASecondPatternWhileItRunsIsRefused) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  EXPECT_FALSE(rig.runner().execute_pattern("square"));
  EXPECT_FALSE(rig.runner().execute_pattern("track"));
  auto* r = dynamic_cast<PatternRunner*>(&rig.runner());
  ASSERT_NE(r, nullptr);
  EXPECT_NE(r->progress().find("track"), std::string::npos) << r->progress();
}

// How long it runs is the run's to say, as in legacy pychron (the script
// passes its duration); the pattern's own is for when the run has none.
TEST(Dragonfly, TheRunsDurationWinsOverThePatterns) {
  Rig rig;
  const TimePoint started = rig.clock.now();
  ASSERT_TRUE(rig.runner().execute_pattern_for("track", 6));  // the file says 20
  auto done = rig.finish();
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_GE(rig.clock.now() - started, Duration(6s));
  EXPECT_LT(rig.clock.now() - started, Duration(8s));

  const TimePoint again = rig.clock.now();
  ASSERT_TRUE(rig.runner().execute_pattern_for("track", 0));  // the run has none: the file's 20
  ASSERT_TRUE(rig.finish());
  EXPECT_GE(rig.clock.now() - again, Duration(20s));

  const TimePoint open = rig.clock.now();
  ASSERT_TRUE(rig.runner().execute_pattern_for("track_open", 4));  // a pattern with none of its own
  ASSERT_TRUE(rig.finish());
  EXPECT_GE(rig.clock.now() - open, Duration(4s));
  EXPECT_LT(rig.clock.now() - open, Duration(6s));
}

TEST(Dragonfly, WithNoDurationAnywhereItIsRefused) {
  Rig rig;
  const auto before = rig.targets().size();
  for (double none : {0.0, -1.0, std::nan("")}) {
    auto r = rig.runner().execute_pattern_for("track_open", none);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
    EXPECT_NE(r.error().what.find("duration"), std::string::npos) << r.error().what;
  }
  auto plain = rig.runner().execute_pattern("track_open");
  ASSERT_FALSE(plain);
  EXPECT_EQ(rig.targets().size(), before);
  EXPECT_FALSE(*rig.runner().running());
  // a path pattern takes no duration and ignores one
  EXPECT_TRUE(rig.runner().execute_pattern_for("square", 999));
}

// The beam is on. With no glow to follow the search spirals outward: never
// further than the room the hole has before its neighbours (45% of the way to
// the nearest), whatever the pattern's own perimeter. Hole 7's neighbours are
// 1.41 mm away.
TEST(Dragonfly, ItsPerimeterIsLimitedToTheRoomItsHoleHas) {
  Rig rig(glow_camera(0, 0), "7");  // (18, 28) on the stage
  rig.shutter->blank = true;        // nothing glows: it will search
  const auto before = rig.targets().size();
  ASSERT_TRUE(rig.runner().execute_pattern("track"));  // the file's perimeter is 2.5 mm
  auto done = rig.finish();
  ASSERT_TRUE(done) << done.error().what;
  const auto moves = rig.targets();
  ASSERT_GT(moves.size(), before + 3);
  const double room = 0.45 * std::sqrt(2.0);
  for (std::size_t i = before; i < moves.size(); ++i) EXPECT_LE(apart(moves[i], {18, 28}), room + 0.001) << i;
  const std::string note = rig.runner().last_note();
  EXPECT_NE(note.find("0.636"), std::string::npos) << note;              // said: it was held in
  EXPECT_NE(note.find("glow was not seen"), std::string::npos) << note;  // and that it followed nothing
}

TEST(Dragonfly, AHoleWithRoomKeepsThePatternsPerimeter) {
  Rig rig(glow_camera(0, 0), "3");  // nearest neighbour 4.47 mm: room 2.01
  rig.shutter->blank = true;
  ASSERT_TRUE(rig.runner().execute_pattern("track_tight"));  // 0.4 mm: tighter than the room
  ASSERT_TRUE(rig.finish());
  double furthest = 0;
  for (const auto& t : rig.targets()) furthest = std::max(furthest, apart(t, {15, 20}));
  EXPECT_LE(furthest, 0.4 + 0.001);
  EXPECT_GT(furthest, 0.3);
  EXPECT_EQ(rig.runner().last_note().find("held within"), std::string::npos);  // its own perimeter: nothing to add
}

// --- the stage or the settings fail with the beam on ---------------------------

TEST(Dragonfly, SettingsThatCannotSeeAreRefusedBeforeItStarts) {
  Rig rig;
  auto r = rig.runner().execute_pattern("track_blind");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("target_radius"), std::string::npos) << r.error().what;
  EXPECT_FALSE(*rig.runner().running());
  EXPECT_EQ(rig.shutter->grabs, 0);
}

TEST(Dragonfly, AMoveTheStageRefusesEndsIt) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  rig.sim.fail_next("Stage.MoveTo", 4);  // its first correction
  auto done = rig.finish();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Io);
  EXPECT_NE(done.error().what.find("pattern track"), std::string::npos) << done.error().what;
  EXPECT_FALSE(*rig.runner().running());  // over: said once
  EXPECT_TRUE(rig.runner().execute_pattern("square"));
}

TEST(Dragonfly, AStageThatCannotSayWhereItIsEndsIt) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  // The first look's three frames are taken (the simulated camera asks the
  // stage where it is for each); the next question is the pattern's own.
  rig.shutter->after_grab = [&](int count) {
    if (count == 3) rig.sim.fail_next("Stage.Pos?", 4);
  };
  auto done = rig.finish();
  ASSERT_FALSE(done);
  EXPECT_NE(done.error().what.find("pattern track"), std::string::npos) << done.error().what;
  EXPECT_FALSE(*rig.runner().running());
}

TEST(Dragonfly, AReturnTheStageRefusesEndsItWithTheError) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(5));
  rig.shutter->dead = true;              // it will turn for home
  rig.sim.fail_next("Stage.MoveTo", 4);  // and the stage will not go
  auto done = rig.finish();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Io);
  EXPECT_NE(done.error().what.find("pattern track"), std::string::npos) << done.error().what;
  EXPECT_FALSE(*rig.runner().running());
}

// The camera is lost and the pattern is then stopped before its time is up:
// the log still says the camera was lost.
TEST(Dragonfly, ACameraLostIsSaidEvenIfThePatternIsStoppedAfter) {
  Rig rig;
  ASSERT_TRUE(rig.runner().execute_pattern("track"));
  ASSERT_TRUE(rig.run_for(5));
  rig.shutter->dead = true;
  ASSERT_TRUE(rig.run_for(4));  // home, and holding
  ASSERT_TRUE(rig.runner().stop_pattern());
  const std::string note = rig.runner().last_note();
  EXPECT_NE(note.find("camera"), std::string::npos) << note;
}
