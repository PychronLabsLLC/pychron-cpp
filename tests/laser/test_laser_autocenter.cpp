// Autocenter after a hole move, and the corrections it leaves (laser
// autocenter design, sections 5 and 6). The laser system on the Chromium
// simulator, with the simulated camera seeing the tray where it really is.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "laser_harness.hpp"

using namespace pychron;
using namespace pychron::extraction;
using namespace pychron::laser;
using namespace pychron::laser::harness;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
using vision::AutocenterReason;
using Outcome = AutocenterOutcome::Result;

namespace {

// Hole 3 of the small tray is at (5, 0) on the map: calibrated at stage
// (15, 20), really at (15.15, 19.90) with the harness's tray error.
constexpr double kTrueX = 15.15, kTrueY = 19.90;

// A frame source that counts, and can fail or freeze.
class CountingSource final : public vision::IFrameSource {
 public:
  CountingSource(std::unique_ptr<vision::IFrameSource> inner, int fail_at = 0, bool freeze = false)
      : inner_(std::move(inner)), fail_at_(fail_at), freeze_(freeze) {}
  Result<vision::Frame> grab() override {
    ++grabs;
    if (fail_at_ > 0 && grabs >= fail_at_) return fail(ErrorKind::Io, "the camera stopped answering");
    auto frame = inner_->grab();
    if (frame && freeze_) frame->timestamp = TimePoint{} + 1s;  // never newer
    return frame;
  }
  vision::FrameInfo info() const override { return inner_->info(); }
  int grabs = 0;

 private:
  std::unique_ptr<vision::IFrameSource> inner_;
  int fail_at_;
  bool freeze_;
};

// A harness whose frames go through a CountingSource.
struct CountedHarness : LaserHarness {
  CorrectionStore corrections{lab.dir / "stage_corrections"};
  CountingSource* frames = nullptr;
  explicit CountedHarness(CameraConfig seen = camera_config(), int fail_at = 0, bool freeze = false) {
    system.set_corrections(corrections);
    auto source = std::make_unique<CountingSource>(std::make_unique<SimTrayCamera>(seen, system.sight(), clock),
                                                   fail_at, freeze);
    frames = source.get();
    system.attach_camera(seen, std::move(source), clock);
    EXPECT_TRUE(system.set_tray("small"));
  }
  Result<bool> drive(int limit = 3000) {
    Result<bool> busy = true;
    for (int i = 0; i < limit; ++i) {
      busy = system.moving();
      if (!busy || !*busy) return busy;
      clock.advance(100ms);
    }
    ADD_FAILURE() << "still moving after " << limit << " polls";
    return busy;
  }
  StageXY at() const {
    const auto p = sim.position();
    return {static_cast<double>(p.x) / 1000.0, static_cast<double>(p.y) / 1000.0};
  }
  std::size_t moves() const {
    std::size_t n = 0;
    for (const auto& line : sim.log()) n += line.starts_with("Stage.MoveTo ") ? 1 : 0;
    return n;
  }
};

HoleCorrections saved(const CorrectionStore& store, const TrayLibrary& trays, const CalibrationStore& calibrations) {
  const TrayMap& map = *trays.find("small");
  auto loaded = store.load(map, "co2", calibrations.status(map, "co2").fingerprint);
  EXPECT_TRUE(loaded);
  return loaded ? *loaded : HoleCorrections{};
}

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(WithACamera, ExtractionDeviceConformance, ::testing::Types<CameraHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(WithACamera, LaserConformance, ::testing::Types<CameraHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(WithACamera, StageConformance, ::testing::Types<CameraHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(WithACamera, PatternConformance, ::testing::Types<CameraHarness>);

TEST(Autocenter, ConvergesOnTheTrueHoleAndSaves) {
  CameraHarness h;
  EXPECT_TRUE(h.system.has_camera());
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::None);
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto done = h.drive();
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_NEAR(h.at().x, kTrueX, 0.03);
  EXPECT_NEAR(h.at().y, kTrueY, 0.03);

  const auto outcome = h.system.last_autocenter();
  EXPECT_EQ(outcome.result, Outcome::Converged);
  EXPECT_EQ(outcome.reason, AutocenterReason::None);
  EXPECT_EQ(outcome.hole, "3");
  EXPECT_EQ(outcome.tray, "small");
  EXPECT_GE(outcome.iterations, 2);  // at least one nudge, and the look that found it centred
  EXPECT_NEAR(outcome.moved_mm.x, 0.15, 0.03);
  EXPECT_NEAR(outcome.moved_mm.y, -0.10, 0.03);
  EXPECT_NEAR(outcome.found.x, kTrueX, 0.03);
  EXPECT_LT(outcome.residual_mm, 0.03);

  const auto on_disk = saved(h.corrections, h.trays, h.store);
  ASSERT_TRUE(on_disk.contains("3"));
  EXPECT_NEAR(on_disk.at("3").x, kTrueX, 0.03);
  EXPECT_NEAR(on_disk.at("3").y, kTrueY, 0.03);
  EXPECT_LT(on_disk.at("3").residual_mm, 0.03);
  EXPECT_EQ(on_disk.at("3").found.size(), 20u);  // 2026-10-04T16:20:11Z
  EXPECT_EQ(h.system.corrections().size(), 1u);
}

TEST(Autocenter, TheNextMoveStartsAtTheCorrection) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.drive());
  const auto found = h.at();
  ASSERT_TRUE(h.system.set_xy(0, 0));
  ASSERT_TRUE(h.drive());

  const auto before = h.stage_moves().size();
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.drive());
  const auto moves = h.stage_moves();
  // one move, straight to where it was found last time; then a look that finds it centred
  ASSERT_EQ(moves.size(), before + 1);
  EXPECT_NEAR(h.at().x, found.x, 1e-9);
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::Converged);
  EXPECT_EQ(h.system.last_autocenter().iterations, 1);
  EXPECT_NEAR(std::hypot(h.system.last_autocenter().moved_mm.x, h.system.last_autocenter().moved_mm.y), 0, 1e-9);
}

TEST(Autocenter, WithoutTheFlagTheCorrectionIsUsedAndNoFrameTaken) {
  CountedHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.drive());
  const auto found = h.at();
  ASSERT_TRUE(h.system.set_xy(0, 0));
  ASSERT_TRUE(h.drive());
  const int grabs = h.frames->grabs;
  ASSERT_GT(grabs, 0);

  ASSERT_TRUE(h.system.move_to_position("3", false));
  ASSERT_TRUE(h.drive());
  EXPECT_EQ(h.frames->grabs, grabs);
  EXPECT_NEAR(h.at().x, found.x, 1e-9);
  EXPECT_NEAR(h.at().y, found.y, 1e-9);

  // a hole never centred goes to its calibrated position
  ASSERT_TRUE(h.system.move_to_position("2", false));
  ASSERT_TRUE(h.drive());
  EXPECT_NEAR(h.at().x, 10, 1e-9);
  EXPECT_NEAR(h.at().y, 25, 1e-9);
}

TEST(Autocenter, WithNoCameraNothingChanges) {
  LaserHarness h;
  EXPECT_FALSE(h.system.has_camera());
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(conformance::settles(h, [&] { return h.system.moving(); }));
  EXPECT_EQ(h.sim.position().x, 15000);
  EXPECT_EQ(h.sim.position().y, 20000);
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::None);
}

TEST(Autocenter, SettleIsWaitedBeforeLooking) {
  CameraConfig c = camera_config();
  c.settle = 1s;
  CountedHarness h(c);
  ASSERT_TRUE(h.system.move_to_position("3", true));
  // drive in small steps until the first frame is taken: by then the stage
  // has been at rest for the settle time
  TimePoint last_motion = h.clock.now();
  auto last_position = h.sim.position();
  for (int i = 0; i < 3000 && h.frames->grabs == 0; ++i) {
    ASSERT_TRUE(h.system.moving());
    if (h.frames->grabs > 0) break;
    h.clock.advance(50ms);
    const auto now = h.sim.position();
    if (now.x != last_position.x || now.y != last_position.y) {
      last_motion = h.clock.now();
      last_position = now;
    }
  }
  ASSERT_GT(h.frames->grabs, 0);
  EXPECT_GE(h.clock.now() - last_motion, Duration(1s));
}

TEST(Autocenter, OnePollSendsAtMostOneStageMove) {
  CountedHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto sent = h.moves();
  for (int i = 0; i < 3000; ++i) {
    auto busy = h.system.moving();
    ASSERT_TRUE(busy);
    ASSERT_LE(h.moves(), sent + 1);
    sent = h.moves();
    if (!*busy) break;
    h.clock.advance(100ms);
  }
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::Converged);
}

TEST(Autocenter, NoHoleInViewReturnsToTheStart) {
  CameraHarness h(camera_config(2.5, 2.5));  // the tray is nowhere near
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto done = h.drive();
  ASSERT_TRUE(done) << done.error().what;  // on_failure = continue: the move ends normally
  EXPECT_FALSE(*done);
  EXPECT_NEAR(h.at().x, 15, 1e-9);
  EXPECT_NEAR(h.at().y, 20, 1e-9);
  const auto outcome = h.system.last_autocenter();
  EXPECT_EQ(outcome.result, Outcome::Failed);
  EXPECT_EQ(outcome.reason, AutocenterReason::NoTarget);
  EXPECT_EQ(outcome.hole, "3");
  EXPECT_TRUE(saved(h.corrections, h.trays, h.store).empty());
  EXPECT_FALSE(*h.system.moving());
}

TEST(Autocenter, FailingIsAnErrorWhenAskedFor) {
  CameraConfig c = camera_config(2.5, 2.5);
  c.on_failure = OnAutocenterFailure::Fail;
  CameraHarness h(c);
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto done = h.drive();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().kind, ErrorKind::Config);
  EXPECT_EQ(done.error().device, "co2");
  for (const char* word : {"hole 3", "small", "no_target"}) {
    EXPECT_NE(done.error().what.find(word), std::string::npos) << done.error().what;
  }
  EXPECT_NEAR(h.at().x, 15, 1e-9);  // back at the start before it said so
  EXPECT_NEAR(h.at().y, 20, 1e-9);
  EXPECT_FALSE(*h.system.moving());  // said once
}

// The system believes the camera is mounted the other way round in x: every
// nudge goes the wrong way and the offset grows.
TEST(Autocenter, AWrongFlipIsCaughtAndNothingIsSaved) {
  CameraConfig believed = camera_config();
  believed.flip_x = true;
  CameraHarness h(believed, camera_config());
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto done = h.drive();
  ASSERT_TRUE(done) << done.error().what;
  const auto outcome = h.system.last_autocenter();
  EXPECT_EQ(outcome.result, Outcome::Failed);
  EXPECT_TRUE(outcome.reason == AutocenterReason::Runaway || outcome.reason == AutocenterReason::MaxTotal ||
              outcome.reason == AutocenterReason::MaxIterations || outcome.reason == AutocenterReason::NoTarget)
      << to_string(outcome.reason);
  EXPECT_NEAR(h.at().x, 15, 1e-9);
  EXPECT_NEAR(h.at().y, 20, 1e-9);
  EXPECT_TRUE(saved(h.corrections, h.trays, h.store).empty());
}

// Hole 7 has neighbours 1.41 mm away on either side: its guard is 0.64 mm.
// Whatever the tray's error along that line, the stage never ends further
// from the calibrated position than the guard, and a hole is only ever
// "found" where hole 7 really is.
struct Along {
  double mm;
  bool must_fail;
  friend void PrintTo(const Along& a, std::ostream* os) { *os << a.mm << " mm"; }
};
class AutocenterGuard : public ::testing::TestWithParam<Along> {};

TEST_P(AutocenterGuard, NeverEndsOnTheNeighbour) {
  const double e = GetParam().mm / std::sqrt(2.0);
  CameraHarness h(camera_config(e, e));
  const double guard = h.system.guard_mm("7");
  EXPECT_NEAR(guard, 0.45 * std::sqrt(2.0), 1e-9);
  ASSERT_TRUE(h.system.move_to_position("7", true));
  auto done = h.drive();
  ASSERT_TRUE(done) << done.error().what;
  const double nominal_x = 18, nominal_y = 28;  // (8, 8) on the map
  EXPECT_LE(std::hypot(h.at().x - nominal_x, h.at().y - nominal_y), guard + 0.011);
  const auto outcome = h.system.last_autocenter();
  if (outcome.result == Outcome::Converged) {
    EXPECT_FALSE(GetParam().must_fail);
    EXPECT_NEAR(h.at().x, nominal_x + e, 0.03);
    EXPECT_NEAR(h.at().y, nominal_y + e, 0.03);
  } else {
    EXPECT_EQ(outcome.result, Outcome::Failed);
    EXPECT_NEAR(h.at().x, nominal_x, 1e-9);
    EXPECT_NEAR(h.at().y, nominal_y, 1e-9);
    EXPECT_TRUE(saved(h.corrections, h.trays, h.store).empty());
  }
}

// 0.70 mm: the true hole and the neighbour are both outside the guard.
INSTANTIATE_TEST_SUITE_P(Errors, AutocenterGuard,
                         ::testing::Values(Along{0.2, false}, Along{0.5, false}, Along{-0.5, false}, Along{0.7, true},
                                           Along{-0.7, true}));

TEST(Autocenter, GuardIsFromTheNearestNeighbour) {
  CameraHarness h;
  EXPECT_DOUBLE_EQ(h.system.guard_mm("3"), 1.0);                     // nothing within 2.2 mm: the cap
  EXPECT_NEAR(h.system.guard_mm("7"), 0.45 * std::sqrt(2.0), 1e-9);  // neighbours 1.41 mm away
  EXPECT_DOUBLE_EQ(h.system.guard_mm("no-such"), 0);
  ASSERT_TRUE(h.system.set_tray(""));
  EXPECT_DOUBLE_EQ(h.system.guard_mm("3"), 0);
}

TEST(Autocenter, ACorrectionOutsideTheGuardIsIgnored) {
  CameraHarness h;
  const TrayMap& map = *h.trays.find("small");
  const auto fp = h.store.status(map, "co2").fingerprint;
  ASSERT_TRUE(h.corrections.put(map, "co2", fp, "3", {18.0, 20.0, 0, "by hand"}));  // 3 mm off
  ASSERT_TRUE(h.corrections.put(map, "co2", fp, "2", {10.2, 25.1, 0, "by hand"}));  // plausible
  ASSERT_TRUE(h.system.set_tray("small"));
  ASSERT_TRUE(h.system.move_to_position("3", false));
  ASSERT_TRUE(h.drive());
  EXPECT_NEAR(h.at().x, 15, 1e-9);
  ASSERT_TRUE(h.system.move_to_position("2", false));
  ASSERT_TRUE(h.drive());
  EXPECT_NEAR(h.at().x, 10.2, 1e-9);
  EXPECT_NEAR(h.at().y, 25.1, 1e-9);
}

TEST(Autocenter, ANewCalibrationDropsTheCorrections) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.drive());
  ASSERT_EQ(h.system.corrections().size(), 1u);
  h.calibrate(11, 21);  // the tray was put back somewhere else
  ASSERT_TRUE(h.system.set_tray("small"));
  EXPECT_TRUE(h.system.corrections().empty());
  ASSERT_TRUE(h.system.move_to_position("3", false));
  ASSERT_TRUE(h.drive());
  EXPECT_NEAR(h.at().x, 16, 1e-9);
  EXPECT_NEAR(h.at().y, 21, 1e-9);
}

TEST(Autocenter, ACameraThatFailsIsAFailure) {
  CountedHarness h(camera_config(), 2);  // its second frame never comes
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto done = h.drive();
  ASSERT_TRUE(done) << done.error().what;
  const auto outcome = h.system.last_autocenter();
  EXPECT_EQ(outcome.result, Outcome::Failed);
  EXPECT_EQ(outcome.reason, AutocenterReason::Camera);
  EXPECT_NEAR(h.at().x, 15, 1e-9);
  EXPECT_NEAR(h.at().y, 20, 1e-9);
}

// Frames that never get newer cannot be trusted after a nudge: it ends, and
// in a bounded number of polls.
TEST(Autocenter, FramesThatNeverGetNewerEndIt) {
  CountedHarness h(camera_config(), 0, true);
  ASSERT_TRUE(h.system.move_to_position("3", true));
  auto done = h.drive(400);
  ASSERT_TRUE(done) << done.error().what;
  EXPECT_FALSE(*done);
  const auto outcome = h.system.last_autocenter();
  EXPECT_EQ(outcome.result, Outcome::Failed);
  EXPECT_EQ(outcome.reason, AutocenterReason::StaleFrame);
  EXPECT_NEAR(h.at().x, 15, 1e-9);
  EXPECT_LT(h.frames->grabs, 60);
}

TEST(Autocenter, StopEndsItAndSavesNothing) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  h.clock.advance(500ms);  // on its way
  ASSERT_TRUE(h.system.stop());
  EXPECT_FALSE(*h.system.moving());
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::Stopped);
  const auto where = h.at();
  for (int i = 0; i < 50; ++i) {
    EXPECT_FALSE(*h.system.moving());
    h.advance();
  }
  EXPECT_NEAR(h.at().x, where.x, 1e-9);  // no look, no nudge, no return
  EXPECT_TRUE(saved(h.corrections, h.trays, h.store).empty());
  // and the next hole move starts clean
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.drive());
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::Converged);
}

TEST(Autocenter, ANewMoveAbandonsIt) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  h.clock.advance(300ms);
  ASSERT_TRUE(*h.system.moving());
  ASSERT_TRUE(h.system.move_to_position("2", true));  // changed its mind
  ASSERT_TRUE(h.drive());
  EXPECT_NEAR(h.at().x, 10.15, 0.03);
  EXPECT_NEAR(h.at().y, 24.90, 0.03);
  EXPECT_EQ(h.system.last_autocenter().hole, "2");
  const auto on_disk = saved(h.corrections, h.trays, h.store);
  EXPECT_TRUE(on_disk.contains("2"));
  EXPECT_FALSE(on_disk.contains("3"));
}

TEST(Autocenter, AMoveToCoordinatesAbandonsIt) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.system.set_xy(1, 1));
  ASSERT_TRUE(h.drive());
  EXPECT_NEAR(h.at().x, 1, 1e-9);
  EXPECT_NEAR(h.at().y, 1, 1e-9);
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::Stopped);
  EXPECT_TRUE(saved(h.corrections, h.trays, h.store).empty());
}

TEST(Autocenter, ChangingTheTrayAbandonsIt) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.system.set_tray("bare"));
  ASSERT_TRUE(h.drive());
  EXPECT_NE(h.system.last_autocenter().result, Outcome::Converged);
  EXPECT_TRUE(saved(h.corrections, h.trays, h.store).empty());
}

// A pattern is run about the centred position.
TEST(Autocenter, APatternStartsFromTheCentredHole) {
  CameraHarness h;
  ASSERT_TRUE(h.system.move_to_position("3", true));
  ASSERT_TRUE(h.drive());
  const auto centre = h.at();
  auto* runner = h.system.pattern_runner();
  ASSERT_NE(runner, nullptr);
  ASSERT_TRUE(runner->execute_pattern("square"));
  for (int i = 0; i < 3000 && *runner->running(); ++i) h.advance();
  EXPECT_NEAR(h.at().x, centre.x, 1e-9);
  EXPECT_NEAR(h.at().y, centre.y, 1e-9);
}

// A scan position is the driver's: no camera business.
TEST(Autocenter, AScanPositionIsNotCentred) {
  CountedHarness h;
  h.sim.add_scan({2000, 3000, 0});
  ASSERT_TRUE(h.system.move_to_position("s1", true));
  ASSERT_TRUE(h.drive());
  EXPECT_EQ(h.frames->grabs, 0);
  EXPECT_EQ(h.system.last_autocenter().result, Outcome::None);
}
