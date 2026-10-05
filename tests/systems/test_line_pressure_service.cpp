// LinePressureService: scripts' get_pressure over the line's gauges
// (plan 2026-10-05, task B0).
#include "pychron/systems/line_pressure_service.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "pychron/core/config/loader.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
using namespace std::chrono_literals;

constexpr const char* kSystem = R"(
[system]
name = "t"
scan_interval_ms = 1000
[transports.gnet]
kind = "sim"
[drivers.bone]
kind = "pfeiffer_maxigauge"
transport = "gnet"
channels = [1]
[[gauges]]
name = "IG"
driver = "bone"
channel = 1
)";

struct Line {
  explicit Line(bool start = true) {
    auto cfg = config::load_system_config_from_string(kSystem, "t.toml");
    EXPECT_TRUE(cfg) << cfg.error().what;
    ExtractionLine::Options o;
    o.clock = &clock;
    o.scheduler.threads = 0;
    o.run_scheduler = false;
    o.sim.noise = 0.0;
    o.sim.default_pressure = 2e-8;
    auto made = ExtractionLine::create(*cfg, std::nullopt, o);
    EXPECT_TRUE(made) << made.error().what;
    line = std::move(*made);
    if (start) EXPECT_TRUE(line->start());
  }
  ~Line() { line->stop(); }

  ManualClock clock;
  std::unique_ptr<ExtractionLine> line;
};

}  // namespace

TEST(LinePressureService, AnswersTheLatestReading) {
  Line l;
  LinePressureService pressure(*l.line);
  auto p = pressure.get_pressure("bone", "IG");
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_DOUBLE_EQ(*p, 2e-8);
  // Any controller, and the manometer form, find the same gauge.
  EXPECT_DOUBLE_EQ(*pressure.get_pressure("", "IG"), 2e-8);
  EXPECT_DOUBLE_EQ(*pressure.get_manometer_pressure("IG"), 2e-8);
}

TEST(LinePressureService, UnknownGaugeOrControllerIsConfig) {
  Line l;
  LinePressureService pressure(*l.line);
  auto unknown = pressure.get_pressure("bone", "CG9");
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().kind, ErrorKind::Config);
  auto elsewhere = pressure.get_pressure("microbone", "IG");
  ASSERT_FALSE(elsewhere);
  EXPECT_EQ(elsewhere.error().kind, ErrorKind::Config);
  EXPECT_NE(elsewhere.error().what.find("is on 'bone'"), std::string::npos) << elsewhere.error().what;
}

TEST(LinePressureService, NoReadingYetIsNotConnected) {
  Line l(/*start=*/false);
  LinePressureService pressure(*l.line);
  auto p = pressure.get_pressure("", "IG");
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::NotConnected);
}

TEST(LinePressureService, AStaleReadingIsNeverPassedOffAsNow) {
  Line l;
  LinePressureService pressure(*l.line);
  l.clock.advance(3s);  // three scan intervals: still fine
  EXPECT_TRUE(pressure.get_pressure("", "IG"));
  l.clock.advance(1500ms);
  auto stale = pressure.get_pressure("", "IG");
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().kind, ErrorKind::Io);
  EXPECT_NE(stale.error().what.find("4.5 s ago"), std::string::npos) << stale.error().what;
  ASSERT_TRUE(l.line->read_gauge("IG"));  // a fresh read clears it
  EXPECT_TRUE(pressure.get_pressure("", "IG"));
}
