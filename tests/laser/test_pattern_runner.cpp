// PatternRunner: a pattern run point by point over the laser system's stage,
// driven by the caller's running() polls (laser patterns design, section 7).
// On the Chromium driver and its simulator.

#include "pychron/laser/pattern_runner.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "extraction/fake.hpp"
#include "laser_harness.hpp"

using namespace pychron;
using namespace pychron::extraction;
using namespace pychron::laser;
using namespace pychron::laser::harness;
using namespace std::chrono_literals;

namespace {

struct PatternRunnerTest : LaserSystemTest {
  IPatternRunner& runner() {
    auto* r = system.pattern_runner();
    if (r == nullptr) throw std::logic_error("no pattern runner");
    return *r;
  }
  // The stage at (x, y), settled.
  void park(double x, double y) {
    ASSERT_TRUE(system.set_xy(x, y));
    settle();
  }
  std::vector<std::string> moves() const {
    std::vector<std::string> out;
    for (const auto& line : sim.log()) {
      if (line.starts_with("Stage.MoveTo ")) out.push_back(line.substr(13));
    }
    return out;
  }
  void finish() { ASSERT_TRUE(conformance::settles(*this, [&] { return runner().running(); })); }
};

}  // namespace

TEST_F(PatternRunnerTest, VisitsThePointsInOrderAtThePatternsSpeed) {
  park(10, 20);
  const auto before = moves().size();
  ASSERT_TRUE(runner().execute_pattern("square"));
  EXPECT_TRUE(*runner().running());
  finish();
  auto made = moves();
  made.erase(made.begin(), made.begin() + static_cast<std::ptrdiff_t>(before));
  // radius 1 about (10, 20), closed, then the center; 2 mm/s along each
  // segment (the diagonals share it between x and y: 1414 each)
  EXPECT_EQ(made, (std::vector<std::string>{"11000,20000,0,2000,2000,100", "10000,21000,0,1414,1414,100",
                                            "9000,20000,0,1414,1414,100", "10000,19000,0,1414,1414,100",
                                            "11000,20000,0,1414,1414,100", "10000,20000,0,2000,2000,100"}));
  EXPECT_FALSE(*runner().running());
  EXPECT_EQ(sim.position().x, 10000);
  EXPECT_EQ(sim.position().y, 20000);
  EXPECT_FALSE(*system.moving());
}

TEST_F(PatternRunnerTest, RepeatsItsIterations) {
  const auto before = moves().size();
  ASSERT_TRUE(runner().execute_pattern("twice"));
  finish();
  EXPECT_EQ(moves().size() - before, 11u);  // two squares of five, and home
}

TEST_F(PatternRunnerTest, OnePollSendsAtMostOneMove) {
  ASSERT_TRUE(runner().execute_pattern("square"));
  auto sent = moves().size();
  for (int i = 0; i < 400; ++i) {
    auto busy = runner().running();
    ASSERT_TRUE(busy);
    ASSERT_LE(moves().size(), sent + 1);
    sent = moves().size();
    if (!*busy) break;
    advance();
  }
  EXPECT_FALSE(*runner().running());
}

TEST_F(PatternRunnerTest, StopHaltsTheStageMidSegment) {
  park(10, 20);
  ASSERT_TRUE(runner().execute_pattern("square"));
  clock.advance(200ms);  // on the way to the first vertex, 1 mm off at 2 mm/s
  ASSERT_TRUE(runner().stop_pattern());
  EXPECT_EQ(sim.log().back(), "Stage.Pos?");  // the stop's confirmation
  EXPECT_TRUE(logged("Stage.Stop"));
  EXPECT_FALSE(*runner().running());
  EXPECT_FALSE(*system.moving());
  const auto at = sim.position();
  EXPECT_GT(at.x, 10000);
  EXPECT_LT(at.x, 11000);
  clock.advance(10s);
  EXPECT_EQ(sim.position().x, at.x);  // it really stopped: no vertex, no return to the center
  const auto sent = moves().size();
  EXPECT_FALSE(*runner().running());
  EXPECT_EQ(moves().size(), sent);
  // and another pattern can start from here
  ASSERT_TRUE(runner().execute_pattern("square"));
  finish();
  EXPECT_EQ(sim.position().x, at.x);
}

TEST_F(PatternRunnerTest, StopWhenIdleIsFine) {
  const auto before = sim.log().size();
  EXPECT_TRUE(runner().stop_pattern());
  EXPECT_FALSE(*runner().running());
  EXPECT_EQ(sim.log().size(), before);  // nothing to stop, nothing sent
}

TEST_F(PatternRunnerTest, APointOutsideTravelEndsThePattern) {
  const auto before = moves().size();
  auto started = runner().execute_pattern("wide");  // its first point is 60 mm out; travel is 50
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Config);
  EXPECT_NE(started.error().what.find("pattern wide, point 1 of 6"), std::string::npos) << started.error().what;
  EXPECT_EQ(moves().size(), before);
  EXPECT_FALSE(*runner().running());
  EXPECT_TRUE(runner().execute_pattern("square"));  // nothing is left half started
}

TEST_F(PatternRunnerTest, ALaterPointOutsideTravelEndsItThere) {
  // about (0, 49.5): the first vertex (1, 49.5) is inside, the second (0, 50.5) is not
  park(0, 49.5);
  const auto sent_before = moves().size();
  ASSERT_TRUE(runner().execute_pattern("square"));  // (1, 49.5) is inside
  Result<bool> busy = true;
  for (int i = 0; i < 400 && busy && *busy; ++i) {
    advance();
    busy = runner().running();
  }
  ASSERT_FALSE(busy);  // (0, 50.5) is outside
  EXPECT_EQ(busy.error().kind, ErrorKind::Config);
  EXPECT_NE(busy.error().what.find("pattern square, point 2 of 6"), std::string::npos) << busy.error().what;
  EXPECT_EQ(moves().size(), sent_before + 1);  // the first vertex, and nothing after the refusal
  EXPECT_FALSE(*runner().running());  // ended: the error is said once
  EXPECT_FALSE(*system.moving());
}

TEST_F(PatternRunnerTest, AFailedMoveEndsThePattern) {
  ASSERT_TRUE(runner().execute_pattern("square"));
  // let the first two moves go, then Chromium refuses the third
  Result<bool> busy = true;
  const auto start = moves().size();
  while (moves().size() < start + 1) {
    advance();
    busy = runner().running();
    ASSERT_TRUE(busy);
  }
  sim.fail_next("Stage.MoveTo", 4);
  for (int i = 0; i < 400 && busy && *busy; ++i) {
    advance();
    busy = runner().running();
  }
  ASSERT_FALSE(busy);
  EXPECT_EQ(busy.error().kind, ErrorKind::Io);
  EXPECT_NE(busy.error().what.find("pattern square, point 3 of 6"), std::string::npos) << busy.error().what;
  EXPECT_FALSE(*runner().running());
}

TEST_F(PatternRunnerTest, ASecondPatternWhileRunningIsRefused) {
  ASSERT_TRUE(runner().execute_pattern("square"));
  const auto sent = moves().size();
  auto second = runner().execute_pattern("twice");
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().kind, ErrorKind::Config);
  EXPECT_NE(second.error().what.find("square"), std::string::npos) << second.error().what;
  EXPECT_EQ(moves().size(), sent);
  finish();
  EXPECT_EQ(moves().size(), sent + 5);  // the first went on to its end
}

// The center is where the stage is: while it is still going somewhere, that
// is nowhere in particular.
TEST_F(PatternRunnerTest, APatternIsNotStartedWhileTheStageMoves) {
  ASSERT_TRUE(system.set_xy(10, 0));  // under way, not waited for
  const auto sent = moves().size();
  auto r = runner().execute_pattern("square");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("moving"), std::string::npos) << r.error().what;
  EXPECT_EQ(moves().size(), sent);
  EXPECT_FALSE(*runner().running());
  settle();
  EXPECT_TRUE(runner().execute_pattern("square"));
}

// It runs only while it is polled: a caller that will not wait must know.
TEST_F(PatternRunnerTest, SaysItNeedsPolling) { EXPECT_TRUE(runner().needs_polling()); }

TEST_F(PatternRunnerTest, UnknownAndBrokenPatternsAreConfig) {
  const auto before = sim.log().size();
  auto unknown = runner().execute_pattern("no-such");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_EQ(unknown.error().device, "co2");
  EXPECT_NE(unknown.error().what.find("no-such"), std::string::npos);
  EXPECT_NE(unknown.error().what.find("square"), std::string::npos) << unknown.error().what;  // says what there is

  auto broken = runner().execute_pattern("broken");
  ASSERT_FALSE(broken);
  EXPECT_EQ(broken.error().kind, ErrorKind::Config);
  EXPECT_NE(broken.error().what.find("radius"), std::string::npos) << broken.error().what;  // the file's problem
  EXPECT_EQ(sim.log().size(), before);
  EXPECT_EQ(runner().patterns(), (std::vector<std::string>{"square", "track", "track_blind", "track_brief", "track_open", "track_tight", "twice", "walk", "wide"}));
}

TEST_F(PatternRunnerTest, ASeededWalkRepeats) {
  ASSERT_TRUE(runner().execute_pattern("walk"));
  finish();
  const auto first = moves();
  ASSERT_EQ(first.size(), 5u);
  ASSERT_TRUE(runner().execute_pattern("walk"));
  finish();
  const auto both = moves();
  ASSERT_EQ(both.size(), 10u);
  for (std::size_t i = 0; i < 5; ++i) EXPECT_EQ(both[i + 5], first[i]);
}

TEST_F(PatternRunnerTest, ProgressSaysWhereItIs) {
  // what a watcher is told (the runner itself is behind the system's gate)
  EXPECT_EQ(system.snapshot().pattern_progress, "");
  ASSERT_TRUE(runner().execute_pattern("square"));
  EXPECT_EQ(system.snapshot().pattern_progress, "square, point 1 of 6");
  finish();
  EXPECT_EQ(system.snapshot().pattern_progress, "");
}

// A position that cannot be read is no center to run a pattern about.
TEST_F(PatternRunnerTest, WithNoPositionNothingStarts) {
  sim.fail_next("Stage.Pos?", 4);
  const auto before = moves().size();
  auto r = runner().execute_pattern("square");
  ASSERT_FALSE(r);
  EXPECT_EQ(moves().size(), before);
  EXPECT_FALSE(*runner().running());
}

TEST(PatternRunnerFeatures, NeedsAStageAndALibrary) {
  LabDir lab;
  const auto trays = TrayLibrary::load(lab.dir / "tray_maps");
  const CalibrationStore store{lab.dir / "stage_calibrations"};
  const auto patterns = PatternLibrary::load(lab.dir / "patterns");
  ASSERT_FALSE(patterns.names().empty());

  extraction::testing::FakeExtractionDevice staged{"staged", {Capability::Stage}};
  LaserSystem without{"staged", staged, trays, store};
  EXPECT_EQ(without.pattern_runner(), nullptr);  // no library
  LaserSystem with{"staged", staged, trays, store, &patterns};
  EXPECT_NE(with.pattern_runner(), nullptr);

  extraction::testing::FakeExtractionDevice furnace{"furnace", {Capability::Furnace}};
  LaserSystem stageless{"furnace", furnace, trays, store, &patterns};
  EXPECT_EQ(stageless.pattern_runner(), nullptr);

  // a device that runs patterns itself keeps doing so
  extraction::testing::FakeExtractionDevice own{"own", {Capability::Stage, Capability::Pattern}};
  LaserSystem theirs{"own", own, trays, store, &patterns};
  ASSERT_NE(theirs.pattern_runner(), nullptr);
  EXPECT_EQ(theirs.pattern_runner()->patterns(), own.pattern_runner()->patterns());
  EXPECT_FALSE(theirs.pattern_runner()->needs_polling()) << "the device's own runner, not the system's";
}

// A stage that cannot stop: stopping the pattern still ends it, at the next
// vertex at the latest.
TEST(PatternRunnerFeatures, AStageThatCannotStopStillEndsThePattern) {
  LabDir lab;
  const auto trays = TrayLibrary::load(lab.dir / "tray_maps");
  const CalibrationStore store{lab.dir / "stage_calibrations"};
  const auto patterns = PatternLibrary::load(lab.dir / "patterns");
  extraction::testing::FakeExtractionDevice staged{"staged", {Capability::Stage}};
  LaserSystem system{"staged", staged, trays, store, &patterns};
  auto* runner = system.pattern_runner();
  ASSERT_NE(runner, nullptr);
  ASSERT_TRUE(runner->execute_pattern("square"));
  EXPECT_TRUE(runner->stop_pattern());
  EXPECT_FALSE(*runner->running());
}
