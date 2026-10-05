// The line's cryostat: [cryo], the scan, and the scripts' cryo commands
// (plan 2026-10-05, tasks C1 and C4).
#include "pychron/systems/line_cryo_service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <vector>

#include "pychron/core/config/loader.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
using namespace std::chrono_literals;

constexpr const char* kSystem = R"(
[system]
name = "t"
scan_interval_ms = 1000
[transports.cryo]
kind = "sim"
[drivers.cryostat]
kind = "lakeshore"
transport = "cryo"
ranges = [
  { output = 1, range = 1, min = 0.0, max = 30.0 },
  { output = 1, range = 3, min = 30.0, max = 400.0 },
  { output = 2, range = 2, min = 0.0, max = 400.0 },
]
[cryo]
driver = "cryostat"
tolerance_k = 0.5
timeout_s = 300
[cryo.setpoints]
He_freeze = [14.0, 20.0]
Ar_freeze = [90.0]
)";

struct Line {
  explicit Line(const char* toml = kSystem) {
    auto cfg = config::load_system_config_from_string(toml, "t.toml");
    EXPECT_TRUE(cfg) << cfg.error().what;
    ExtractionLine::Options o;
    o.clock = &clock;
    o.scheduler.threads = 0;
    o.run_scheduler = false;
    auto made = ExtractionLine::create(*cfg, std::nullopt, o);
    EXPECT_TRUE(made) << made.error().what;
    line = std::move(*made);
    samples_sub = line->bus().subscribe<TemperatureSample>([this](const TemperatureSample& s) {
      std::lock_guard lock(m);
      samples.push_back(s);
    });
  }
  ~Line() { line->stop(); }

  ManualClock clock;
  std::unique_ptr<ExtractionLine> line;
  std::mutex m;
  std::vector<TemperatureSample> samples;
  SignalBus::Subscription samples_sub;
};

}  // namespace

TEST(CryoConfig, LoadsAndChecks) {
  auto cfg = config::load_system_config_from_string(kSystem, "t.toml");
  ASSERT_TRUE(cfg) << cfg.error().what;
  ASSERT_TRUE(cfg->cryo);
  EXPECT_EQ(cfg->cryo->driver, "cryostat");
  EXPECT_DOUBLE_EQ(cfg->cryo->tolerance_k, 0.5);
  EXPECT_EQ(cfg->cryo->setpoints.at("He_freeze"), (std::vector<double>{14.0, 20.0}));
  const std::string base = "[system]\nname = \"t\"\n[transports.c]\nkind = \"sim\"\n[drivers.c]\nkind = \"lakeshore\"\ntransport = \"c\"\n";
  EXPECT_FALSE(config::load_system_config_from_string(base + "[cryo]\ndriver = \"nope\"\n", "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(base + "[cryo]\ndriver = \"c\"\ntimeout_s = 0\n", "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(base + "[cryo]\ndriver = \"c\"\nstray = 1\n", "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(base + "[cryo]\ndriver = \"c\"\n[cryo.setpoints]\nx = [-1.0]\n",
                                                      "t.toml"));
  EXPECT_FALSE(config::load_system_config_from_string(base + "[cryo]\ndriver = \"c\"\n[cryo.setpoints]\nx = []\n",
                                                      "t.toml"));
}

TEST(LineCryo, TheLineScansEveryInputAndPublishesIt) {
  Line l;
  ASSERT_TRUE(l.line->start());
  {
    std::lock_guard lock(l.m);
    ASSERT_EQ(l.samples.size(), 2u);  // A and B, read at start
    EXPECT_EQ(l.samples[0].source, "cryostat");
    EXPECT_EQ(l.samples[0].input, "A");
    EXPECT_NEAR(l.samples[0].kelvin, 293.15, 0.01);
  }
  ASSERT_TRUE(l.line->latest_temperature("B"));
  l.clock.advance(1s);
  l.line->scheduler().run_pending();
  std::lock_guard lock(l.m);
  EXPECT_EQ(l.samples.size(), 4u);
}

TEST(LineCryo, ADriverThatIsNotAControllerFailsStart) {
  Line l(R"(
[system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.relay]
kind = "proxr_relay"
transport = "bus"
[cryo]
driver = "relay"
)");
  auto r = l.line->start();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
  EXPECT_NE(r.error().what.find("not a temperature controller"), std::string::npos);
}

TEST(LineCryo, SetCryoSetsOutputOneAndSettlesWithinTolerance) {
  Line l;
  ASSERT_TRUE(l.line->start());
  LineCryoService cryo(*l.line);
  EXPECT_EQ(*cryo.cryo_settling(), false);  // nothing set: nothing to wait for
  ASSERT_TRUE(cryo.set_cryo(77.0));
  EXPECT_DOUBLE_EQ(*l.line->cryostat()->setpoint(1), 77.0);
  EXPECT_EQ(*cryo.cryo_settling(), true);  // still at room temperature
  l.clock.advance(10min);
  EXPECT_EQ(*cryo.cryo_settling(), false);
  EXPECT_NEAR(*cryo.get_cryo_temp(1), 77.0, 0.5);
}

TEST(LineCryo, ANamedSetpointGoesOutputByOutput) {
  Line l;
  ASSERT_TRUE(l.line->start());
  LineCryoService cryo(*l.line);
  ASSERT_TRUE(cryo.set_cryo_named("He_freeze"));
  EXPECT_DOUBLE_EQ(*l.line->cryostat()->setpoint(1), 14.0);
  EXPECT_DOUBLE_EQ(*l.line->cryostat()->setpoint(2), 20.0);
  auto unknown = cryo.set_cryo_named("freeze");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  EXPECT_NE(unknown.error().what.find("Ar_freeze, He_freeze"), std::string::npos) << unknown.error().what;
}

TEST(LineCryo, AWaitThatOutlastsTheTimeoutFails) {
  // Legacy waited forever.
  Line l;
  ASSERT_TRUE(l.line->start());
  LineCryoService cryo(*l.line);
  ASSERT_TRUE(cryo.set_cryo(77.0));
  l.clock.advance(1s);
  EXPECT_EQ(*cryo.cryo_settling(), true);
  // Hold the input warm, as a coldhead that has failed would.
  auto* tc = l.line->cryostat();
  ASSERT_TRUE(tc->set_setpoint(1, 293.0));  // the controller now holds it warm
  l.clock.advance(301s);
  auto r = cryo.cryo_settling();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Io);
  EXPECT_NE(r.error().what.find("did not reach its setpoint within 300 s"), std::string::npos) << r.error().what;
}

TEST(LineCryo, GetCryoTempChannelsAreTheControllersInputs) {
  Line l;
  ASSERT_TRUE(l.line->start());
  LineCryoService cryo(*l.line);
  EXPECT_TRUE(cryo.get_cryo_temp(2));
  auto r = cryo.get_cryo_temp(3);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Config);
}
