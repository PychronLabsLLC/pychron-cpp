#include "pychron/systems/extraction_line.hpp"

#include <chrono>
#include <filesystem>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/config/loader.hpp"
#include "pychron/systems/canvas/loader.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
using namespace std::chrono_literals;

constexpr const char* kSystem = R"(
[system]
name = "t"
scan_interval_ms = 1000

[transports.bus]
kind = "sim"

[transports.gnet]
kind = "sim"

[drivers.relay]
kind = "proxr_relay"
transport = "bus"

[drivers.ig]
kind = "pfeiffer_maxigauge"
transport = "gnet"
channels = [1]

[[valves]]
name = "A"
actuator = "relay"
address = "1"

[[valves]]
name = "B"
actuator = "relay"
address = "2"

[[manual_valves]]
name = "M1"

[[gauges]]
name = "IG1"
driver = "ig"
channel = 1
units = "torr"
)";

constexpr const char* kCanvas = R"(
[[valve]]
name = "A"
pos = [0, 0]

[[manual_valve]]
name = "M1"
pos = [0, 0]

[[gauge]]
name = "IG1"
pos = [0, 0]

[[stage]]
name = "left"
pos = [0, 0]
volume = 3.0

[[stage]]
name = "right"
pos = [0, 0]

[[connection]]
start = "left"
end = "A"

[[connection]]
start = "A"
end = "right"

[[connection]]
start = "right"
end = "IG1"
)";

config::SystemConfig system_config(const char* toml = kSystem) {
  auto cfg = config::load_system_config_from_string(toml, "t.toml");
  EXPECT_TRUE(cfg) << cfg.error().what;
  return *cfg;
}

canvas::Canvas canvas_model(const char* toml = kCanvas) {
  auto c = canvas::load_canvas_from_string(toml, "c.toml");
  EXPECT_TRUE(c) << c.error().what;
  return *c;
}

ExtractionLine::Options manual(const Clock& clock) {
  ExtractionLine::Options o;
  o.clock = &clock;
  o.scheduler.threads = 0;
  o.run_scheduler = false;
  o.sim.noise = 0.0;
  o.sim.default_pressure = 1e-8;
  return o;
}

TEST(ExtractionLine, BuildsManagersFromConfig) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), canvas_model(), manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  auto& l = **line;
  EXPECT_NE(l.transport("bus"), nullptr);
  EXPECT_NE(l.device("relay"), nullptr);
  EXPECT_EQ(l.device("nope"), nullptr);
  EXPECT_TRUE(l.switches().contains("A"));
  EXPECT_TRUE(l.switches().contains("M1"));
  ASSERT_NE(l.network(), nullptr);
  EXPECT_TRUE(l.network()->is_valve("A"));
  ASSERT_NE(l.sim(), nullptr);
  EXPECT_TRUE(l.sim()->has_volume("left"));
  EXPECT_FALSE(l.running());
}

TEST(ExtractionLine, CanvasNamingUnknownValveIsConfigError) {
  ManualClock clock;
  std::string bad = std::string(kCanvas) + "\n[[valve]]\nname = \"Z\"\npos = [0, 0]\n";
  auto line = ExtractionLine::create(system_config(), canvas_model(bad.c_str()), manual(clock));
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
  EXPECT_NE(line.error().what.find("Z"), std::string::npos);
}

TEST(ExtractionLine, SystemValveMissingFromCanvasIsWarning) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), canvas_model(), manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  // B is configured but not drawn.
  bool found = false;
  for (const auto& w : (*line)->warnings()) found = found || w.message.find("'B'") != std::string::npos;
  EXPECT_TRUE(found);
}

TEST(ExtractionLine, RunsWithoutCanvas) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), std::nullopt, manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ((*line)->network(), nullptr);
  ASSERT_TRUE((*line)->start());
  auto p = (*line)->read_gauge("IG1");
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, 1e-8, 1e-8 * 1e-3);
}

TEST(ExtractionLine, UnknownDriverKindIsConfigError) {
  ManualClock clock;
  auto cfg = system_config();
  cfg.drivers.at("ig").kind = "mystery";
  auto line = ExtractionLine::create(std::move(cfg), std::nullopt, manual(clock));
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
}

TEST(ExtractionLine, StartPublishesFullSnapshot) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), canvas_model(), manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  std::vector<Snapshot> snaps;
  auto sub = (*line)->bus().subscribe<Snapshot>([&](const Snapshot& s) { snaps.push_back(s); });

  ASSERT_TRUE((*line)->start());
  EXPECT_TRUE((*line)->running());
  ASSERT_EQ(snaps.size(), 1u);
  EXPECT_EQ(snaps[0].valves.at("A"), ValveState::Closed);
  EXPECT_EQ(snaps[0].valves.at("B"), ValveState::Closed);
  EXPECT_EQ(snaps[0].valves.at("M1"), ValveState::Unknown);  // manual: operator has not reported
  ASSERT_TRUE(snaps[0].pressures.contains("IG1"));
  EXPECT_NEAR(snaps[0].pressures.at("IG1"), 1e-8, 1e-11);

  // Idempotent start: no second snapshot.
  ASSERT_TRUE((*line)->start());
  EXPECT_EQ(snaps.size(), 1u);
}

TEST(ExtractionLine, ScansPublishPressureSamples) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), canvas_model(), manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  std::vector<PressureSample> samples;
  auto sub = (*line)->bus().subscribe<PressureSample>([&](const PressureSample& s) { samples.push_back(s); });
  ASSERT_TRUE((*line)->start());

  clock.advance(1s);
  (*line)->scheduler().run_pending();
  (*line)->scheduler().wait_idle();
  ASSERT_EQ(samples.size(), 1u);
  EXPECT_EQ(samples[0].gauge, "IG1");

  (*line)->stop();
  EXPECT_FALSE((*line)->running());
  clock.advance(5s);
  (*line)->scheduler().run_pending();
  EXPECT_EQ(samples.size(), 1u);
}

TEST(ExtractionLine, ActuateOpensValveAndMovesGas) {
  ManualClock clock;
  auto opts = manual(clock);
  opts.sim.initial_pressures = {{"left", 4e-6}};
  auto line = ExtractionLine::create(system_config(), canvas_model(), opts);
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());

  std::vector<ValveChanged> changes;
  auto sub = (*line)->bus().subscribe<ValveChanged>([&](const ValveChanged& e) { changes.push_back(e); });
  ASSERT_TRUE((*line)->actuate("A", SwitchOp::Open, "test"));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].state, ValveState::Open);
  EXPECT_TRUE((*line)->sim()->valve_open("A"));

  // left (3 cc @ 4e-6) + right (1 cc) + IG1 (1 cc) @ 1e-8
  auto p = (*line)->read_gauge("IG1");
  ASSERT_TRUE(p) << p.error().what;
  EXPECT_NEAR(*p, (3 * 4e-6 + 2e-8) / 5, 1e-9);
  EXPECT_EQ((*line)->snapshot().valves.at("A"), ValveState::Open);
}

TEST(ExtractionLine, ManualValveReportReachesSim) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), canvas_model(), manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());
  ASSERT_TRUE((*line)->actuate("M1", SwitchOp::Open, "op"));
  EXPECT_TRUE((*line)->sim()->valve_open("M1"));
}

TEST(ExtractionLine, UnknownGaugeIsConfigError) {
  ManualClock clock;
  auto line = ExtractionLine::create(system_config(), canvas_model(), manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  auto p = (*line)->read_gauge("nope");
  ASSERT_FALSE(p);
  EXPECT_EQ(p.error().kind, ErrorKind::Config);
}

TEST(ExtractionLine, TransportOpenFailureFailsStartAndClosesAll) {
  ManualClock clock;
  std::string toml = std::string(kSystem);
  toml.replace(toml.find("[transports.bus]\nkind = \"sim\""), std::string("[transports.bus]\nkind = \"sim\"").size(),
               "[transports.bus]\nkind = \"serial\"\nport = \"/dev/pychron-no-such-port\"");
  auto line = ExtractionLine::create(system_config(toml.c_str()), std::nullopt, manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  auto started = (*line)->start();
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, ErrorKind::Io);
  EXPECT_NE(started.error().what.find("bus"), std::string::npos);
  EXPECT_FALSE((*line)->running());
}

TEST(ExtractionLine, ForceSimReplacesHardwareTransports) {
  ManualClock clock;
  std::string toml = std::string(kSystem);
  toml.replace(toml.find("[transports.bus]\nkind = \"sim\""), std::string("[transports.bus]\nkind = \"sim\"").size(),
               "[transports.bus]\nkind = \"serial\"\nport = \"/dev/pychron-no-such-port\"");
  auto opts = manual(clock);
  opts.force_sim = true;
  auto line = ExtractionLine::create(system_config(toml.c_str()), std::nullopt, opts);
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());
  EXPECT_TRUE((*line)->actuate("A", SwitchOp::Open, "t"));
}

TEST(ExtractionLine, NoSimTransportsMeansNoSimSystem) {
  ManualClock clock;
  std::string toml = std::string(kSystem);
  for (const char* t : {"[transports.bus]\nkind = \"sim\"", "[transports.gnet]\nkind = \"sim\""}) {
    std::string from = t;
    std::string to = from.substr(0, from.find('\n')) + "\nkind = \"tcp\"\nhost = \"127.0.0.1\"\nport = 1";
    toml.replace(toml.find(from), from.size(), to);
  }
  auto line = ExtractionLine::create(system_config(toml.c_str()), std::nullopt, manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ((*line)->sim(), nullptr);
}

TEST(ExtractionLine, LoadsExampleFiles) {
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  ManualClock clock;
  auto line = ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_TRUE((*line)->switches().contains("pump_power"));
  EXPECT_NE((*line)->canvas(), nullptr);
}

TEST(ExtractionLine, LoadReportsMissingFile) {
  auto line = ExtractionLine::load("/nonexistent/extraction_line.toml", std::nullopt);
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
}

}  // namespace

TEST(ExtractionLine, TransportWireLogsWhenHubAndTraceSet) {
  ManualClock clock;
  auto dir = std::filesystem::temp_directory_path() / "pychron_line_wire_test";
  std::filesystem::create_directories(dir);

  auto cfg = system_config();
  cfg.transports["gnet"].trace = true;

  auto opts = manual(clock);
  opts.trace_dir = dir;
  config::LoggingConfig lc;
  lc.default_level = LogLevel::Trace;

  // The hub logs to its own bus so the test can observe the wire records.
  SignalBus hub_bus;
  std::vector<Log> wire;
  auto sub = hub_bus.subscribe<Log>([&](const Log& e) {
    if (e.logger == "gnet.wire") wire.push_back(e);
  });
  auto hub = LogHub::create(lc, clock, &hub_bus);
  ASSERT_TRUE(hub);
  opts.log_hub = *hub;

  {
    auto line = ExtractionLine::create(std::move(cfg), canvas_model(), opts);
    ASSERT_TRUE(line) << line.error().what;
    ASSERT_TRUE((*line)->start());
    ASSERT_TRUE((*line)->read_gauge("IG1"));
    (*line)->stop();
  }
  EXPECT_FALSE(wire.empty());
  std::filesystem::remove_all(dir);
}
