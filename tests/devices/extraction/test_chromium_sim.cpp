// ChromiumSim: the laser PC as the vendor reference describes it. Queries
// answer with a CR-terminated line, actions answer nothing, errors are ?<n>,
// and the stage takes distance / speed on the injected clock.

#include "pychron/devices/extraction/chromium_sim.hpp"

#include <chrono>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "pychron/core/clock.hpp"

using namespace pychron;
using namespace std::chrono_literals;
using extraction::ChromiumSim;

namespace {

struct ChromiumSimTest : ::testing::Test {
  ManualClock clock;
  ChromiumSim sim{clock};
  SimTransport::Hook hook = sim.hook();

  std::string ask(std::string_view command) { return to_string(hook(to_bytes(std::string(command) + "\n"))); }
};

}  // namespace

TEST_F(ChromiumSimTest, QueriesAnswerAndActionsAreSilent) {
  EXPECT_EQ(ask("Sys.ID?"), "CHROMIUM 2013.12.30.0\r");
  EXPECT_EQ(ask("laser.enable 1"), "");  // case-insensitive, silent
  EXPECT_EQ(ask("Laser.Enable?"), "1\r");
  EXPECT_TRUE(sim.enabled());
  EXPECT_EQ(ask("Laser.Output 12.5"), "");
  EXPECT_EQ(ask("Laser.Output?"), "12.5\r");
  EXPECT_EQ(sim.output(), 12.5);
  EXPECT_EQ(ask("Laser.Status?"), "0\r");
  EXPECT_EQ(ask("Laser.Interlocks?"), "\r");
  EXPECT_EQ(ask("Laser.Fire"), "");
  EXPECT_TRUE(sim.firing());
  EXPECT_EQ(ask("Laser.Stop"), "");
  EXPECT_FALSE(sim.firing());
  ask("Laser.Fire");
  EXPECT_EQ(ask("Laser.Enable 0"), "");  // disabling stops the beam
  EXPECT_FALSE(sim.firing());
  EXPECT_EQ(ask("Scans.Status_Verbosity 1"), "");
  EXPECT_EQ(sim.log().front(), "Sys.ID?");
  EXPECT_EQ(sim.log().at(1), "laser.enable 1");  // as sent, without the LF
}

TEST_F(ChromiumSimTest, ErrorsUseTheVendorCodes) {
  EXPECT_EQ(ask("Lazer.Fire"), "?1\r");
  EXPECT_EQ(ask("Laser.Explode"), "?2\r");
  EXPECT_EQ(ask("Laser.Output"), "?3\r");
  EXPECT_EQ(ask("Laser.Output abc"), "?3\r");
  EXPECT_EQ(ask("Laser.Output 100.5"), "?3\r");
  EXPECT_EQ(ask("Stage.MoveTo 1,2,3"), "?3\r");
  EXPECT_EQ(ask("Laser.Fire"), "?4\r");  // not enabled
  ask("Laser.Enable 1");
  sim.trip_interlock("Door");
  EXPECT_EQ(ask("Laser.Status?"), "1\r");
  EXPECT_EQ(ask("Laser.Interlocks?"), "Door\r");
  EXPECT_EQ(ask("Laser.Fire"), "?4\r");
  EXPECT_FALSE(sim.firing());
  sim.clear_interlocks();
  EXPECT_EQ(ask("Laser.Status?"), "0\r");
  sim.fail_next("Laser.Enable", 4);
  EXPECT_EQ(ask("Laser.Enable 1"), "?4\r");
  EXPECT_EQ(ask("Laser.Enable 1"), "");  // once only
  sim.set_id("NGX 1.0");
  EXPECT_EQ(ask("Sys.ID?"), "NGX 1.0\r");
}

TEST_F(ChromiumSimTest, TheStageMovesAtTheCommandedSpeedAndStops) {
  EXPECT_EQ(ask("Stage.Pos?"), "0,0,0\r");
  EXPECT_EQ(ask("Stage.MoveTo 10000,-5000,0,5000,5000,100"), "");
  clock.advance(1s);
  EXPECT_EQ(ask("Stage.Pos?"), "5000,-5000,0\r");  // x half way, y there
  clock.advance(1s);
  EXPECT_EQ(ask("Stage.Pos?"), "10000,-5000,0\r");
  clock.advance(10s);
  EXPECT_EQ(ask("Stage.Pos?"), "10000,-5000,0\r");  // exactly on target, and stays
  ask("Stage.MoveTo 0,-5000,0,5000,0,0");
  clock.advance(500ms);
  EXPECT_EQ(ask("Stage.Stop"), "");
  clock.advance(5s);
  EXPECT_EQ(ask("Stage.Pos?"), "7500,-5000,0\r");
  EXPECT_EQ(sim.position(), (codec::chromium::Microns{7500, -5000, 0}));
  // a speed of 0 leaves that axis where it is
  ask("Stage.MoveTo 0,0,900,5000,5000,0");
  clock.advance(10s);
  EXPECT_EQ(ask("Stage.Pos?"), "0,0,0\r");
}

TEST_F(ChromiumSimTest, ScansAreNumberedFromOne) {
  EXPECT_EQ(ask("Scans.Count?"), "0\r");
  sim.add_scan({2000, 3000, 0});
  EXPECT_EQ(ask("Scans.Count?"), "1\r");
  EXPECT_EQ(ask("Scans.InPos? 1"), "0\r");
  EXPECT_EQ(ask("Scans.MoveTo 1"), "");
  EXPECT_EQ(ask("Scans.InPos? 1"), "0\r");  // on its way
  clock.advance(2s);
  EXPECT_EQ(ask("Scans.InPos? 1"), "1\r");
  EXPECT_EQ(ask("Scans.MoveTo 2"), "?3\r");
  EXPECT_EQ(ask("Scans.MoveTo 0"), "?3\r");
  EXPECT_EQ(ask("Scans.InPos? 2"), "?3\r");
  EXPECT_EQ(ask("Scans.Stop"), "");
  sim.put_on_limit('y', -1);
  EXPECT_EQ(ask("Stage.Status?"), "0,-1,0\r");
  sim.put_on_limit('y', 0);
  EXPECT_EQ(ask("Stage.Status?"), "0,0,0\r");
}
