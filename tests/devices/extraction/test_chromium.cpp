// ChromiumLaser against ChromiumSim: the driver as an extraction device, a
// laser and (later in the file) a stage. The simulator answers as the
// vendor's command reference says a Chromium does; see
// docs/superpowers/specs/2026-10-04-chromium-protocol-survey.md.

#include "pychron/devices/extraction/chromium.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>

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
  return o;
}

// Declared in the order they must be built and, reversed, torn down: the
// driver holds the transport, the transport's hook holds the simulator.
struct ChromiumHarness {
  ManualClock clock;
  ChromiumSim sim{clock};
  std::unique_ptr<SimTransport> wire = SimTransport::hooked(sim.hook(), TransportOptions{.name = "laser_pc", .clock = &clock});
  ChromiumLaser laser{"co2", *wire, options()};

  ChromiumHarness() { EXPECT_TRUE(wire->open()); }
  IExtractionDevice& device() { return laser; }
  void advance() { clock.advance(250ms); }
};

struct ChromiumTest : ::testing::Test, ChromiumHarness {
  bool logged(std::string_view command) const {
    const auto log = sim.log();
    return std::find(log.begin(), log.end(), command) != log.end();
  }
};

}  // namespace

INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, ExtractionDeviceConformance, ::testing::Types<ChromiumHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Chromium, LaserConformance, ::testing::Types<ChromiumHarness>);

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
