// ChromiumLaser against ChromiumSim: the driver as an extraction device, a
// laser and (later in the file) a stage. The simulator answers as the
// vendor's command reference says a Chromium does; see
// docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md.

#include "pychron/devices/extraction/chromium.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "extraction/conformance.hpp"
#include "pychron/core/clock.hpp"
#include "pychron/devices/extraction/chromium_sim.hpp"
#include "pychron/transport/sim_transport.hpp"

using namespace pychron;
using namespace pychron::extraction;
using namespace std::chrono_literals;

namespace {

ChromiumOptions options() {
  ChromiumOptions o;
  o.limits_mm = {{{-50, 50}, {-50, 50}, {-50, 50}}};
  return o;
}

// One tray, two holes: what the laser system's tray maps will supply.
TrayLookup lookup() {
  TrayLookup l;
  l.find = [](std::string_view tray, std::string_view position) -> std::optional<StagePosition> {
    if (tray != "221-hole") return std::nullopt;
    if (position == "1") return StagePosition{1.5, -2.0, 0};
    if (position == "2") return StagePosition{10, 10, 0};
    return std::nullopt;
  };
  l.names = [](std::string_view tray) {
    return tray == "221-hole" ? std::vector<std::string>{"1", "2"} : std::vector<std::string>{};
  };
  return l;
}

// Declared in the order they must be built and, reversed, torn down: the
// driver holds the transport, the transport's hook holds the simulator.
struct ChromiumHarness {
  ManualClock clock;
  ChromiumSim sim{clock};
  std::unique_ptr<SimTransport> wire = SimTransport::hooked(sim.hook(), TransportOptions{.name = "laser_pc", .clock = &clock});
  ChromiumLaser laser{"co2", *wire, options()};

  ChromiumHarness() {
    EXPECT_TRUE(wire->open());
    laser.set_tray_lookup(lookup());
    EXPECT_TRUE(laser.set_tray("221-hole"));
  }
  IExtractionDevice& device() { return laser; }
  void advance() { clock.advance(250ms); }
};

struct ChromiumTest : ::testing::Test, ChromiumHarness {
  bool logged(std::string_view command) const {
    const auto log = sim.log();
    return std::find(log.begin(), log.end(), command) != log.end();
  }
  void settle() { ASSERT_TRUE(conformance::settles(*this, [&] { return laser.moving(); })); }
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, ExtractionDeviceConformance, ::testing::Types<ChromiumHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, LaserConformance, ::testing::Types<ChromiumHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, StageConformance, ::testing::Types<ChromiumHarness>);

TEST_F(ChromiumTest, PrepareIdentifiesChromium) {
  EXPECT_TRUE(laser.chromium_id().empty());
  ASSERT_TRUE(laser.prepare());
  EXPECT_EQ(laser.chromium_id(), "CHROMIUM 2013.12.30.0");
  EXPECT_EQ(sim.log().front(), "Sys.ID?");
  EXPECT_TRUE(logged("Scans.Status_Verbosity 1"));
}

TEST_F(ChromiumTest, PrepareRejectsAnythingThatIsNotChromium) {
  sim.set_id("NGX 1.0");
  auto r = laser.prepare();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Protocol);
  EXPECT_NE(r.error().what.find("NGX 1.0"), std::string::npos);
  EXPECT_EQ(r.error().device, "co2");
}

TEST_F(ChromiumTest, EnableRefusesWithATrippedInterlockAndNamesIt) {
  sim.trip_interlock("Door");
  auto r = laser.enable();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("Door"), std::string::npos);
  EXPECT_FALSE(sim.enabled());
  EXPECT_EQ(*laser.is_enabled(), false);
}

TEST_F(ChromiumTest, EnableIsConfirmedOnTheWire) {
  ASSERT_TRUE(laser.enable());
  EXPECT_TRUE(sim.enabled());
  EXPECT_TRUE(logged("Laser.Enable 1"));
  EXPECT_TRUE(logged("Laser.Enable?"));
}

TEST_F(ChromiumTest, ExtractSetsAndConfirmsTheOutput) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(12.5, ExtractUnits::Percent));
  EXPECT_EQ(sim.output(), 12.5);
  EXPECT_EQ(*laser.output(), 12.5);
  EXPECT_FALSE(sim.firing());  // extract sets the output; fire_laser opens the beam
  EXPECT_TRUE(logged("Laser.Output?"));
}

TEST_F(ChromiumTest, OutputAbove100OrNotFiniteIsConfig) {
  ASSERT_TRUE(laser.enable());
  const auto sent = sim.log().size();
  for (double bad : {100.1, std::nan("")}) {
    auto r = laser.extract(bad, ExtractUnits::Percent);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Config);
  }
  EXPECT_EQ(sim.output(), 0.0);
  EXPECT_EQ(sim.log().size(), sent);  // nothing went out
}

TEST_F(ChromiumTest, PercentIsTheOnlyUnit) {
  EXPECT_TRUE(laser.supports(ExtractUnits::Percent));
  EXPECT_FALSE(laser.supports(ExtractUnits::Watts));
  EXPECT_FALSE(laser.supports(ExtractUnits::Celsius));
}

TEST_F(ChromiumTest, ARefusedActionIsAnErrorNotSilence) {
  ASSERT_TRUE(laser.enable());
  sim.fail_next("Laser.Output", 4);
  auto r = laser.extract(10, ExtractUnits::Percent);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_EQ(r.error().code, "chromium?4");
  EXPECT_NE(r.error().what.find("Laser.Output 10"), std::string::npos);
  EXPECT_EQ(r.error().device, "co2");
  EXPECT_EQ(*laser.output(), 0.0);
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));  // the wire is in step again
  EXPECT_EQ(sim.output(), 10.0);
}

TEST_F(ChromiumTest, FireChecksInterlocksEveryTime) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));
  ASSERT_TRUE(laser.fire_laser());
  EXPECT_TRUE(sim.firing());
  ASSERT_TRUE(laser.stop_laser());
  sim.trip_interlock("Coolant");
  auto r = laser.fire_laser();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Interlock);
  EXPECT_NE(r.error().what.find("Coolant"), std::string::npos);
  EXPECT_FALSE(*laser.is_firing());
  EXPECT_FALSE(sim.firing());
}

// A line nobody is waiting for (here: the reply to something written behind
// the driver's back) must not be taken for the next command's reply.
TEST_F(ChromiumTest, StaleInputIsDiscardedBeforeAQuery) {
  ASSERT_TRUE(wire->write(to_bytes("Lazer.Fire\n")));  // leaves "?1\r" unread
  ASSERT_TRUE(laser.prepare());
  EXPECT_EQ(laser.chromium_id(), "CHROMIUM 2013.12.30.0");
}

TEST_F(ChromiumTest, StaleInputIsDiscardedBeforeAnAction) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(wire->write(to_bytes("Lazer.Fire\n")));
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));
  EXPECT_EQ(sim.output(), 10.0);
}

TEST_F(ChromiumTest, AUnitWithoutLaserEnableWorksWithUseEnableOff) {
  ChromiumOptions o = options();
  o.use_enable = false;
  ChromiumLaser plain{"co2", *wire, o};
  ASSERT_TRUE(plain.enable());
  EXPECT_TRUE(*plain.is_enabled());
  ASSERT_TRUE(plain.disable());
  EXPECT_FALSE(*plain.is_enabled());
  const auto log = sim.log();
  EXPECT_TRUE(std::none_of(log.begin(), log.end(), [](const std::string& c) { return c.starts_with("Laser.Enable"); }));
}

TEST_F(ChromiumTest, DisableStopsFiringZeroesOutputAndDisables) {
  ASSERT_TRUE(laser.enable());
  ASSERT_TRUE(laser.extract(10, ExtractUnits::Percent));
  ASSERT_TRUE(laser.fire_laser());
  ASSERT_TRUE(laser.disable());
  EXPECT_FALSE(sim.firing());
  EXPECT_EQ(sim.output(), 0.0);
  EXPECT_FALSE(sim.enabled());
  EXPECT_TRUE(logged("Scans.Stop"));
  EXPECT_EQ(*laser.output(), 0.0);
}

TEST_F(ChromiumTest, HealthFollowsTheWire) {
  ASSERT_TRUE(laser.prepare());
  EXPECT_EQ(laser.health().state, DeviceState::Ok);
  wire->drop_next(3);
  EXPECT_FALSE(laser.prepare());
  EXPECT_NE(laser.health().state, DeviceState::Ok);
}

// --- stage ----------------------------------------------------------------------

TEST_F(ChromiumTest, TheDeviceHasAStageAndNoPatternRunner) {
  EXPECT_EQ(laser.stage(), static_cast<IStage*>(&laser));
  EXPECT_EQ(laser.pattern_runner(), nullptr);
}

TEST_F(ChromiumTest, AnXyMoveKeepsZAndSendsIntegerMicrons) {
  ASSERT_TRUE(laser.set_axis(IStage::Axis::Z, 0.5));
  settle();
  ASSERT_TRUE(laser.set_xy(1.5, -2.0));
  EXPECT_EQ(sim.log().back(), "Stage.Pos?");  // the confirm
  EXPECT_TRUE(logged("Stage.MoveTo 1500,-2000,500,5000,5000,100"));
}

TEST_F(ChromiumTest, MoveOutsideLimitsIsConfigAndSendsNothing) {
  const auto before = sim.log().size();
  EXPECT_EQ(laser.set_xy(50.001, 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(laser.set_xy(0, -50.001).error().kind, ErrorKind::Config);
  EXPECT_EQ(laser.set_axis(IStage::Axis::Z, -50.5).error().kind, ErrorKind::Config);
  EXPECT_EQ(laser.set_xy(std::nan(""), 0).error().kind, ErrorKind::Config);
  EXPECT_EQ(sim.log().size(), before);
  EXPECT_NE(laser.set_xy(50.001, 0).error().what.find("x"), std::string::npos);
  ASSERT_TRUE(laser.set_xy(50, -50));  // the limits themselves are in range
}

TEST_F(ChromiumTest, MovingNeedsThreeGoodPollsInARow) {
  ASSERT_TRUE(laser.set_xy(1.0, 0));  // 1000 microns at 5000 per second
  EXPECT_TRUE(*laser.moving());       // not there yet
  clock.advance(1s);                  // there
  EXPECT_TRUE(*laser.moving());
  EXPECT_TRUE(*laser.moving());
  EXPECT_FALSE(*laser.moving());      // the third in a row
  const auto n = sim.log().size();
  EXPECT_FALSE(*laser.moving());      // no target: nothing sent
  EXPECT_EQ(sim.log().size(), n);
}

TEST_F(ChromiumTest, LimitSwitchWhileMovingIsAnError) {
  ASSERT_TRUE(laser.set_xy(10, 0));
  sim.put_on_limit('x', +1);
  auto r = laser.moving();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("x"), std::string::npos);
  EXPECT_NE(r.error().what.find("positive"), std::string::npos);
  EXPECT_FALSE(*laser.moving());  // target cleared
}

TEST_F(ChromiumTest, SignsAreAppliedBothWays) {
  ChromiumOptions o = options();
  o.signs = {-1, 1, 1};
  ChromiumLaser flipped{"co2", *wire, o};
  ASSERT_TRUE(flipped.set_xy(2.0, 3.0));
  EXPECT_TRUE(logged("Stage.MoveTo -2000,3000,0,5000,5000,100"));
  clock.advance(5s);
  auto at = flipped.position();
  ASSERT_TRUE(at);
  EXPECT_NEAR(at->x, 2.0, 1e-9);
  EXPECT_NEAR(at->y, 3.0, 1e-9);
}

TEST_F(ChromiumTest, AScanPositionMovesByScanNumber) {
  sim.add_scan({2000, 3000, 0});
  ASSERT_TRUE(laser.move_to_position("s1", false));
  EXPECT_TRUE(logged("Scans.MoveTo 1"));
  EXPECT_TRUE(*laser.moving());
  clock.advance(5s);
  EXPECT_TRUE(*laser.moving());
  EXPECT_TRUE(*laser.moving());
  EXPECT_FALSE(*laser.moving());
  // a scan Chromium does not have: its ?3 is a Config error
  auto missing = laser.move_to_position("S9", false);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind, ErrorKind::Config);
  EXPECT_FALSE(*laser.moving());
}

TEST_F(ChromiumTest, AHolePositionIsLookedUpOnTheCurrentTray) {
  ASSERT_TRUE(laser.move_to_position("2", true));  // autocenter is accepted
  EXPECT_TRUE(logged("Stage.MoveTo 10000,10000,0,5000,5000,100"));
  EXPECT_EQ(laser.move_to_position("3", false).error().kind, ErrorKind::Config);
}

TEST_F(ChromiumTest, AnUnknownTrayIsConfig) {
  EXPECT_EQ(laser.set_tray("no-such-tray").error().kind, ErrorKind::Config);
  EXPECT_EQ(laser.positions(), (std::vector<std::string>{"1", "2"}));  // unchanged
}

TEST_F(ChromiumTest, WithNoTrayLookupOnlyScansAndCoordinatesWork) {
  ChromiumLaser bare{"co2", *wire, options()};
  EXPECT_TRUE(bare.positions().empty());
  EXPECT_EQ(bare.move_to_position("1", false).error().kind, ErrorKind::Config);
  EXPECT_TRUE(bare.set_xy(1, 1));
}
