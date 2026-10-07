#include "pychron/systems/extraction_line.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"
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
  opts.sim.default_volume_cc = 1.0;  // right and IG1, which the canvas gives no size
  auto line = ExtractionLine::create(system_config(), canvas_model(), opts);
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->start());

  std::vector<ValveChanged> changes;
  auto sub = (*line)->bus().subscribe<ValveChanged>([&](const ValveChanged& e) { changes.push_back(e); });
  ASSERT_TRUE((*line)->actuate("A", SwitchOp::Open, "test"));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].state, ValveState::Open);
  EXPECT_TRUE((*line)->sim()->valve_open("A"));

  // left (3 cc @ 4e-6) + right (1 cc) + IG1 (1 cc) @ 1e-8, once the gas is
  // across A: a second is eighty of its time constants.
  clock.advance(1s);
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
  auto opts = manual(clock);
  // Not the valve states of somebody's run in the examples folder.
  opts.state_file = std::filesystem::temp_directory_path() / "pychron-test-example-files.state.toml";
  std::filesystem::remove(opts.state_file);
  auto line = ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", opts);
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_TRUE((*line)->switches().contains("pump_power"));
  EXPECT_NE((*line)->canvas(), nullptr);
}

TEST(ExtractionLine, LoadReportsMissingFile) {
  auto line = ExtractionLine::load("/nonexistent/extraction_line.toml", std::nullopt);
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
}

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

TEST(ExtractionLine, BuildsOwnHubFromLoggingConfigAndWireLogs) {
  ManualClock clock;
  auto dir = std::filesystem::temp_directory_path() / "pychron_line_ownhub_test";
  std::filesystem::create_directories(dir);

  auto cfg = system_config();
  cfg.transports["gnet"].trace = true;
  cfg.logging.dir = dir / "logs";
  cfg.logging.default_level = LogLevel::Trace;

  auto opts = manual(clock);
  opts.trace_dir = dir;
  {
    auto line = ExtractionLine::create(std::move(cfg), canvas_model(), opts);
    ASSERT_TRUE(line) << line.error().what;
    ASSERT_NE((*line)->log_hub(), nullptr);
    std::vector<Log> wire;
    auto sub = (*line)->bus().subscribe<Log>([&](const Log& e) {
      if (e.logger == "gnet.wire") wire.push_back(e);
    });
    ASSERT_TRUE((*line)->start());
    ASSERT_TRUE((*line)->read_gauge("IG1"));
    (*line)->stop();
    EXPECT_FALSE(wire.empty());
  }
  EXPECT_TRUE(std::filesystem::exists(dir / "logs" / "pychron.log"));
  std::filesystem::remove_all(dir);
}

// A gauge on a driver that is not a gauge: start() logs the failed initial
// read (then fails to schedule it), which exercises ExtractionLine::log.
constexpr const char* kBadGauge = R"(
[[gauges]]
name = "BAD"
driver = "relay"
channel = 1
units = "torr"
)";

std::size_t lines_containing(const std::filesystem::path& p, const std::string& needle) {
  std::ifstream in(p);
  std::size_t n = 0;
  for (std::string line; std::getline(in, line);)
    if (line.find(needle) != std::string::npos) ++n;
  return n;
}

TEST(ExtractionLine, LineWarningReachesLogFileAndBusOnce) {
  ManualClock clock;
  auto dir = std::filesystem::temp_directory_path() / "pychron_line_log_test";
  std::filesystem::remove_all(dir);

  auto cfg = system_config((std::string(kSystem) + kBadGauge).c_str());
  cfg.logging.dir = dir;
  {
    auto line = ExtractionLine::create(std::move(cfg), std::nullopt, manual(clock));
    ASSERT_TRUE(line) << line.error().what;
    ASSERT_NE((*line)->log_hub(), nullptr);
    std::vector<Log> got;
    auto sub = (*line)->bus().subscribe<Log>([&](const Log& e) {
      if (e.logger == "extraction_line") got.push_back(e);
    });
    (void)(*line)->start();
    (*line)->log_hub()->flush();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].level, LogLevel::Warn);
    EXPECT_NE(got[0].message.find("initial read of gauge 'BAD' failed"), std::string::npos);
    EXPECT_EQ(lines_containing(dir / "pychron.log", "[warn] extraction_line: initial read of gauge 'BAD' failed"),
              1u);
  }
  std::filesystem::remove_all(dir);
}

TEST(ExtractionLine, ActuationAndFailureAreLoggedOnceToBusAndFile) {
  ManualClock clock;
  auto dir = std::filesystem::temp_directory_path() / "pychron_line_act_log";
  std::filesystem::remove_all(dir);
  auto cfg = system_config();
  cfg.logging.dir = dir;
  {
    auto line = ExtractionLine::create(std::move(cfg), std::nullopt, manual(clock));
    ASSERT_TRUE(line) << line.error().what;
    std::vector<Log> got;
    auto sub = (*line)->bus().subscribe<Log>([&](const Log& e) {
      if (e.logger == "switches") got.push_back(e);
    });
    ASSERT_TRUE((*line)->start());
    (*line)->log_hub()->flush();
    got.clear();  // start-up read-back reports initial states
    ASSERT_TRUE((*line)->actuate("A", SwitchOp::Open, "test"));
    (*line)->log_hub()->flush();
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].level, LogLevel::Info);
    EXPECT_EQ(got[0].message, "valve A open");
    EXPECT_EQ(lines_containing(dir / "pychron.log", "[info] switches: valve A open"), 1u);

    got.clear();
    ASSERT_TRUE((*line)->set_locked("A", true));
    EXPECT_FALSE((*line)->actuate("A", SwitchOp::Close, "test"));
    (*line)->log_hub()->flush();
    bool locked_info = false, failed_warn = false;
    for (const auto& r : got) {
      if (r.level == LogLevel::Info && r.message == "valve A locked") locked_info = true;
      if (r.level == LogLevel::Warn && r.message.find("actuate 'A' failed") != std::string::npos) failed_warn = true;
    }
    EXPECT_TRUE(locked_info);
    EXPECT_TRUE(failed_warn);
    EXPECT_EQ(lines_containing(dir / "pychron.log", "[warn] switches: actuate 'A' failed"), 1u);
  }
  std::filesystem::remove_all(dir);
}

TEST(ExtractionLine, InjectedHubOnAnotherBusStillFeedsLineBus) {
  ManualClock clock;
  SignalBus hub_bus;
  int on_hub_bus = 0;
  auto hub_sub = hub_bus.subscribe<Log>([&](const Log& e) {
    if (e.logger == "extraction_line") ++on_hub_bus;
  });
  auto hub = LogHub::create(config::LoggingConfig{}, clock, &hub_bus);
  ASSERT_TRUE(hub);
  auto opts = manual(clock);
  opts.log_hub = *hub;

  auto line = ExtractionLine::create(system_config((std::string(kSystem) + kBadGauge).c_str()), std::nullopt, opts);
  ASSERT_TRUE(line) << line.error().what;
  int on_line_bus = 0;
  auto sub = (*line)->bus().subscribe<Log>([&](const Log& e) {
    if (e.logger == "extraction_line") ++on_line_bus;
  });
  (void)(*line)->start();
  EXPECT_EQ(on_line_bus, 1);
  EXPECT_EQ(on_hub_bus, 1);
}

// A lab in a directory of its own: the line, its canvas, and whatever else
// a test writes beside them.
class SimLab {
 public:
  explicit SimLab(const std::string& name) : dir_(std::filesystem::temp_directory_path() / ("pychron-sim-lab-" + name)) {
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    write("extraction_line.toml", kSystem);
    write("canvas.toml", kCanvas);
  }
  ~SimLab() { std::filesystem::remove_all(dir_); }

  std::filesystem::path write(const std::string& file, const std::string& text) const {
    std::ofstream(dir_ / file, std::ios::binary) << text;
    return dir_ / file;
  }
  auto load(const ExtractionLine::Options& options) const {
    return ExtractionLine::load(dir_ / "extraction_line.toml", dir_ / "canvas.toml", options);
  }
  const std::filesystem::path& dir() const { return dir_; }

 private:
  std::filesystem::path dir_;
};

TEST(ExtractionLine, LoadsSimTomlBesideTheLine) {
  ManualClock clock;
  SimLab lab("beside");
  lab.write("sim.toml", "[volumes.left]\npressure = 4e-6\n[volumes.IG1]\nvolume_cc = 2\n[defaults]\nvolume_cc = 1\n");
  auto opts = manual(clock);
  opts.sim.initial_pressures = {{"left", 9e-3}, {"right", 2e-7}};  // the file's left goes before this one
  auto line = lab.load(opts);
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_DOUBLE_EQ(*(*line)->sim()->pressure("left"), 4e-6);
  ASSERT_TRUE((*line)->start());
  ASSERT_TRUE((*line)->actuate("A", SwitchOp::Open, "test"));
  clock.advance(1s);
  // left (3 cc, the canvas's) at 4e-6; right (1 cc) and IG1 (2 cc), one
  // volume, at the options' 2e-7 and the default 1e-8.
  EXPECT_NEAR(*(*line)->sim()->pressure("right"), (3 * 4e-6 + 1 * 2e-7 + 2 * 1e-8) / 6, 1e-9);
}

TEST(ExtractionLine, SimFileIsTheOneNamedOrNone) {
  ManualClock clock;
  SimLab lab("named");
  lab.write("sim.toml", "[volumes.left]\npressure = 4e-6\n");
  lab.write("other.toml", "[volumes.left]\npressure = 5e-5\n");

  // `[sim] file`, relative to the line's file, goes before the one beside it.
  lab.write("extraction_line.toml", std::string(kSystem) + "\n[sim]\nfile = \"other.toml\"\n");
  auto named = lab.load(manual(clock));
  ASSERT_TRUE(named) << named.error().what;
  EXPECT_DOUBLE_EQ(*(*named)->sim()->pressure("left"), 5e-5);

  // What the caller names goes before both; named and empty, there is none.
  auto opts = manual(clock);
  opts.sim_file = lab.dir() / "sim.toml";
  auto given = lab.load(opts);
  ASSERT_TRUE(given) << given.error().what;
  EXPECT_DOUBLE_EQ(*(*given)->sim()->pressure("left"), 4e-6);
  opts.sim_file = std::filesystem::path{};
  auto none = lab.load(opts);
  ASSERT_TRUE(none) << none.error().what;
  EXPECT_NEAR(*(*none)->sim()->pressure("left"), 1e-8, 1e-20);

  // A file named and not there is an error; one not named and not there is not.
  lab.write("extraction_line.toml", std::string(kSystem) + "\n[sim]\nfile = \"absent.toml\"\n");
  auto absent = lab.load(manual(clock));
  ASSERT_FALSE(absent);
  EXPECT_EQ(absent.error().kind, ErrorKind::Config);
  EXPECT_NE(absent.error().what.find("absent.toml"), std::string::npos) << absent.error().what;
  // `[sim]` knows its keys.
  auto typo = config::load_system_config_from_string((std::string(kSystem) + "\n[sim]\nfiel = \"x\"\n"), "t.toml");
  ASSERT_FALSE(typo);
  EXPECT_NE(typo.error().what.find("sim.fiel"), std::string::npos) << typo.error().what;
}

// One line file serves a tool that loads no canvas (elctl laser) as well:
// the lab's sim.toml speaks of canvas names, and is not that line's.
TEST(ExtractionLine, ALineLoadedWithoutItsCanvasDoesNotReadTheLabsSimToml) {
  ManualClock clock;
  SimLab lab("no-canvas");
  lab.write("sim.toml", "[volumes.left]\npressure = 4e-6\n[defaults]\npressure = 3e-7\n");
  auto beside = ExtractionLine::load(lab.dir() / "extraction_line.toml", std::nullopt, manual(clock));
  ASSERT_TRUE(beside) << beside.error().what;
  EXPECT_DOUBLE_EQ(*(*beside)->sim()->pressure("IG1"), 1e-8) << "the options' pressure, not the file's";

  lab.write("extraction_line.toml", std::string(kSystem) + "\n[sim]\nfile = \"sim.toml\"\n");
  auto named = ExtractionLine::load(lab.dir() / "extraction_line.toml", std::nullopt, manual(clock));
  ASSERT_TRUE(named) << named.error().what;
  EXPECT_DOUBLE_EQ(*(*named)->sim()->pressure("IG1"), 1e-8);

  // What the caller names is read, and checked: the gauge is all there is.
  auto opts = manual(clock);
  opts.sim_file = lab.dir() / "sim.toml";
  EXPECT_FALSE(ExtractionLine::load(lab.dir() / "extraction_line.toml", std::nullopt, opts));
  opts.sim_file = lab.write("gauge.toml", "[volumes.IG1]\npressure = 4e-6\n");
  auto given = ExtractionLine::load(lab.dir() / "extraction_line.toml", std::nullopt, opts);
  ASSERT_TRUE(given) << given.error().what;
  EXPECT_DOUBLE_EQ(*(*given)->sim()->pressure("IG1"), 4e-6);
}

TEST(ExtractionLine, ABadSimTomlFailsTheLoadNamingIt) {
  ManualClock clock;
  SimLab lab("bad");
  const auto file = lab.write("sim.toml", "[defaults]\nnoise = 0\n\n[volumes.nosuch]\nleak = 0\n");
  auto line = lab.load(manual(clock));
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
  EXPECT_NE(line.error().what.find(file.generic_string() + ":4:volumes.nosuch"), std::string::npos)
      << line.error().what;

  lab.write("sim.toml", "[pumps.right]\nspeed = -3\n");
  auto negative = lab.load(manual(clock));
  ASSERT_FALSE(negative);
  EXPECT_NE(negative.error().what.find("sim.toml:2:pumps.right.speed"), std::string::npos) << negative.error().what;
}

// A gauge the canvas does not draw is a volume of its own once its
// controller is simulated: sim.toml may name it, and nothing else that is
// not on the canvas.
TEST(ExtractionLine, SimTomlMayNameAGaugeOffTheCanvas) {
  ManualClock clock;
  SimLab lab("gauge");
  const std::string no_gauge_drawn = std::string(kCanvas).substr(0, std::string(kCanvas).find("[[connection]]\nstart = \"right\"\nend = \"IG1\""));
  std::string canvas = no_gauge_drawn;
  const std::string drawn = "[[gauge]]\nname = \"IG1\"\npos = [0, 0]\n";
  ASSERT_NE(canvas.find(drawn), std::string::npos);
  canvas.erase(canvas.find(drawn), drawn.size());
  lab.write("canvas.toml", canvas);
  lab.write("sim.toml", "[volumes.IG1]\npressure = 1e-3\nvolume_cc = 20\n[pumps.IG1]\nspeed = 0.02\nbase = 1e-9\n");
  auto line = lab.load(manual(clock));
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->sim()->has_volume("IG1"));
  EXPECT_DOUBLE_EQ(*(*line)->sim()->pressure("IG1"), 1e-3);
  clock.advance(1s);  // 0.02 L/s on 20 cc: one time constant
  const double expected = 1e-9 + (1e-3 - 1e-9) * std::exp(-1.0);
  EXPECT_NEAR(*(*line)->sim()->pressure("IG1"), expected, expected * 1e-9);

  lab.write("sim.toml", "[pumps.M1]\nspeed = 1\n");
  EXPECT_FALSE(lab.load(manual(clock))) << "a valve is no volume";
}

TEST(ExtractionLine, ALineTheSimulatorRefusesFailsTheLoad) {
  ManualClock clock;
  auto opts = manual(clock);
  opts.sim.initial_pressures = {{"left", -1e-6}};
  auto line = ExtractionLine::create(system_config(), canvas_model(), opts);
  ASSERT_FALSE(line);
  EXPECT_EQ(line.error().kind, ErrorKind::Config);
  EXPECT_NE(line.error().what.find("left"), std::string::npos) << line.error().what;

  // And a gauge off the canvas whose volume it refuses.
  opts.sim.initial_pressures = {{"IG1", -1e-6}};
  auto gauge = ExtractionLine::create(system_config(), std::nullopt, opts);
  ASSERT_FALSE(gauge);
  EXPECT_NE(gauge.error().what.find("IG1"), std::string::npos) << gauge.error().what;
}

// What the line logs while it is built, through a hub of the test's: every
// warning, and every record at info.
struct BuildLog {
  explicit BuildLog(const Clock& clock) {
    sub = bus.subscribe<Log>([this](const Log& e) {
      if (e.logger != "extraction_line") return;
      if (e.level == LogLevel::Warn) warnings.push_back(e.message);
      if (e.level == LogLevel::Info) infos.push_back(e.message);
      if (e.message.find("carry no gas") != std::string::npos) no_gas.push_back(e);
    });
    auto made = LogHub::create(config::LoggingConfig{}, clock, &bus);
    EXPECT_TRUE(made);
    if (made) hub = *made;
  }
  int count(const std::string& text) const {
    int n = 0;
    for (const auto& w : warnings) n += w.find(text) != std::string::npos ? 1 : 0;
    return n;
  }
  SignalBus bus;
  std::vector<std::string> warnings;
  std::vector<std::string> infos;
  std::vector<Log> no_gas;  // the report of valves with no physics, at whatever level
  SignalBus::Subscription sub;
  std::shared_ptr<LogHub> hub;
};

// Every transport real: there is no simulated lab, so a sim.toml beside the
// line is nobody's. It is not read (a malformed one does not fail the load,
// one naming a volume the canvas lacks is not checked), and the log says
// nothing of the simulator.
TEST(ExtractionLine, ARealLineIgnoresASimTomlBesideIt) {
  ManualClock clock;
  std::string real = std::string(kSystem);
  for (const char* t : {"[transports.bus]\nkind = \"sim\"", "[transports.gnet]\nkind = \"sim\""}) {
    const std::string from = t;
    const std::string to = from.substr(0, from.find('\n')) + "\nkind = \"tcp\"\nhost = \"127.0.0.1\"\nport = 1";
    ASSERT_NE(real.find(from), std::string::npos);
    real.replace(real.find(from), from.size(), to);
  }
  for (const char* sim_toml : {"[defaults\nthis is not toml = = =\n", "[volumes.nosuch]\npressure = 4e-6\n"}) {
    SimLab lab("real");
    lab.write("extraction_line.toml", real);
    lab.write("sim.toml", sim_toml);
    BuildLog log(clock);
    auto opts = manual(clock);
    opts.log_hub = log.hub;
    auto line = lab.load(opts);
    ASSERT_TRUE(line) << sim_toml << ": " << line.error().what;
    EXPECT_EQ((*line)->sim(), nullptr) << sim_toml;
    log.hub->flush();
    for (const auto* said : {&log.infos, &log.warnings}) {
      for (const auto& message : *said) {
        EXPECT_EQ(message.find("sim"), std::string::npos) << sim_toml << ": " << message;
      }
    }
    // The same two files are a simulated line's business: it refuses both.
    opts.force_sim = true;
    EXPECT_FALSE(lab.load(opts)) << sim_toml;
  }
}

// A simulated line loaded without its canvas says which sim file it left
// alone, once, at info: the one beside it and the one `[sim] file` names
// alike.
TEST(ExtractionLine, ALineLoadedWithoutItsCanvasSaysWhichSimTomlItSkipped) {
  ManualClock clock;
  SimLab lab("skipped");
  const auto file = lab.write("sim.toml", "[volumes.left]\npressure = 4e-6\n");
  const std::string expected =
      "sim: " + file.generic_string() + " not read: the line was loaded without its canvas";
  const auto said = [&](const std::optional<std::filesystem::path>& canvas) {
    BuildLog log(clock);
    auto opts = manual(clock);
    opts.log_hub = log.hub;
    auto line = ExtractionLine::load(lab.dir() / "extraction_line.toml", canvas, opts);
    EXPECT_TRUE(line) << line.error().what;
    log.hub->flush();
    EXPECT_EQ(log.count("not read"), 0) << "it is no warning";
    std::vector<std::string> lines;
    for (const auto& message : log.infos) {
      if (message.find("not read") != std::string::npos) lines.push_back(message);
    }
    return lines;
  };

  // Beside the line, not named.
  EXPECT_EQ(said(std::nullopt), std::vector<std::string>{expected});
  // With the canvas it is read, and there is nothing to say.
  EXPECT_TRUE(said(lab.dir() / "canvas.toml").empty());
  // Named by `[sim] file`: the same line.
  lab.write("extraction_line.toml", std::string(kSystem) + "\n[sim]\nfile = \"sim.toml\"\n");
  EXPECT_EQ(said(std::nullopt), std::vector<std::string>{expected});
  EXPECT_TRUE(said(lab.dir() / "canvas.toml").empty());
}

constexpr const char* kTwoValves = R"(
[system]
name = "t"

[transports.bus]
kind = "sim"

[drivers.relay]
kind = "proxr_relay"
transport = "bus"

[[valves]]
name = "V1"
actuator = "relay"
address = "1"

[[valves]]
name = "V2"
actuator = "relay"
address = "2"
)";

// Pipe drawn straight from one valve to the next is a small volume of its
// own between them, so both valves carry gas.
TEST(ExtractionLine, TwoValvesJoinedDirectlyGetAPipeBetweenThem) {
  ManualClock clock;
  // tank (10 cc) -- V1 -- V2 -- line (10 cc)
  const char* canvas = R"(
[[valve]]
name = "V1"
pos = [0, 0]

[[valve]]
name = "V2"
pos = [0, 0]

[[stage]]
name = "tank"
pos = [0, 0]
volume = 10.0

[[stage]]
name = "line"
pos = [0, 0]
volume = 10.0

[[connection]]
start = "tank"
end = "V1"

[[connection]]
start = "V2"
end = "V1"

[[connection]]
start = "V2"
end = "line"
)";
  BuildLog log(clock);
  auto opts = manual(clock);
  opts.log_hub = log.hub;
  opts.sim.outgassing = 0.0;  // what is in a volume is what was put there
  opts.sim.outgassing_active = 0.0;
  opts.sim.initial_pressures = {{"tank", 1e-4}};
  auto made = ExtractionLine::create(system_config(kTwoValves), canvas_model(canvas), opts);
  ASSERT_TRUE(made) << made.error().what;
  sim::SimSystem& lab = *(*made)->sim();
  EXPECT_TRUE(lab.valves_without_physics().empty());
  EXPECT_TRUE(log.no_gas.empty()) << log.no_gas[0].message;
  EXPECT_TRUE(log.warnings.empty()) << log.warnings[0];
  ASSERT_TRUE(lab.has_volume("V1~V2")) << "named for the two valves, in byte order";
  EXPECT_FALSE(lab.has_volume("V2~V1"));
  EXPECT_NEAR(*lab.pressure("V1~V2"), 1e-8, 1e-20);

  // V1 alone: the pipe fills from the tank, and its size is 1 cc; the line,
  // behind V2, does not change.
  lab.set_valve("V1", true);
  clock.advance(10s);
  const double filled = (10 * 1e-4 + 1 * 1e-8) / 11;
  EXPECT_NEAR(*lab.pressure("V1~V2"), filled, filled * 1e-9);
  EXPECT_NEAR(*lab.pressure("tank"), filled, filled * 1e-9);
  EXPECT_DOUBLE_EQ(*lab.pressure("line"), 1e-8);

  // Both: the tank and the line equilibrate through it.
  lab.set_valve("V2", true);
  clock.advance(10s);
  const double level = (10 * 1e-4 + 1 * 1e-8 + 10 * 1e-8) / 21;
  EXPECT_NEAR(*lab.pressure("tank"), level, level * 1e-9);
  EXPECT_NEAR(*lab.pressure("line"), level, level * 1e-9);
  EXPECT_NEAR(*lab.pressure("V1~V2"), level, level * 1e-9);
}

// A pipe's size is `[defaults] pipe_cc`, or its own by name.
TEST(ExtractionLine, SimTomlSizesAPipeBetweenTwoValves) {
  ManualClock clock;
  SimLab lab("pipe");
  lab.write("extraction_line.toml", kTwoValves);
  lab.write("canvas.toml",
            "[[valve]]\nname = \"V1\"\npos = [0, 0]\n[[valve]]\nname = \"V2\"\npos = [0, 0]\n"
            "[[valve]]\nname = \"V3\"\npos = [0, 0]\n"
            "[[stage]]\nname = \"tank\"\npos = [0, 0]\nvolume = 10.0\n"
            "[[stage]]\nname = \"line\"\npos = [0, 0]\nvolume = 10.0\n"
            "[[connection]]\nstart = \"tank\"\nend = \"V1\"\n[[connection]]\nstart = \"V1\"\nend = \"V2\"\n"
            "[[connection]]\nstart = \"V3\"\nend = \"V2\"\n[[connection]]\nstart = \"V3\"\nend = \"line\"\n");
  lab.write("extraction_line.toml",
            std::string(kTwoValves) + "\n[[valves]]\nname = \"V3\"\nactuator = \"relay\"\naddress = \"3\"\n");
  lab.write("sim.toml", "[defaults]\npipe_cc = 4\noutgassing = 0\n[volumes.tank]\npressure = 1e-4\n"
                        "[volumes.\"V2~V3\"]\nvolume_cc = 2\npressure = 3e-6\n");
  auto opts = manual(clock);
  opts.sim.outgassing_active = 0.0;  // no key of the file's
  auto made = lab.load(opts);
  ASSERT_TRUE(made) << made.error().what;
  sim::SimSystem& sim = *(*made)->sim();
  // Three valves in a row: a pipe each side of the middle one.
  ASSERT_TRUE(sim.has_volume("V1~V2"));
  ASSERT_TRUE(sim.has_volume("V2~V3"));
  EXPECT_TRUE(sim.valves_without_physics().empty());
  EXPECT_DOUBLE_EQ(*sim.pressure("V2~V3"), 3e-6);
  sim.set_valve("V1", true);
  clock.advance(10s);
  const double first = (10 * 1e-4 + 4 * 1e-8) / 14;  // pipe_cc
  EXPECT_NEAR(*sim.pressure("V1~V2"), first, first * 1e-9);
  sim.set_valve("V1", false);
  sim.set_valve("V2", true);
  clock.advance(10s);
  const double second = (4 * first + 2 * 3e-6) / 6;  // its own size
  EXPECT_NEAR(*sim.pressure("V2~V3"), second, second * 1e-9);

  lab.write("sim.toml", "[volumes.\"V1~V3\"]\nvolume_cc = 2\n");
  EXPECT_FALSE(lab.load(manual(clock))) << "no such pipe";
}

// A pipe is named `<a>~<b>`, so a valve with a `~` in its own name could be
// taken for one (valves `A`, `B~C` and `A~B`, `C` make the same pipe name):
// a simulated line refuses the valve, by name. A line that simulates
// nothing has no pipes and takes it.
TEST(ExtractionLine, AValveNamedLikeAPipeIsRefusedByTheSim) {
  ManualClock clock;
  const char* system = R"(
[system]
name = "t"

[transports.bus]
kind = "sim"

[drivers.relay]
kind = "proxr_relay"
transport = "bus"

[[valves]]
name = "V1"
actuator = "relay"
address = "1"

[[valves]]
name = "V1~V2"
actuator = "relay"
address = "2"
)";
  const char* canvas = R"(
[[valve]]
name = "V1"
pos = [0, 0]

[[valve]]
name = "V1~V2"
pos = [0, 0]

[[stage]]
name = "tank"
pos = [0, 0]

[[stage]]
name = "line"
pos = [0, 0]

[[connection]]
start = "tank"
end = "V1"

[[connection]]
start = "V1"
end = "line"

[[connection]]
start = "line"
end = "V1~V2"
)";
  auto made = ExtractionLine::create(system_config(system), canvas_model(canvas), manual(clock));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error().kind, ErrorKind::Config);
  EXPECT_NE(made.error().what.find("'V1~V2'"), std::string::npos) << made.error().what;
  EXPECT_NE(made.error().what.find('~'), std::string::npos) << made.error().what;

  // Not simulated: the same names load.
  std::string real = system;
  real.replace(real.find("kind = \"sim\""), 12, "kind = \"tcp\"\nhost = \"127.0.0.1\"\nport = 1");
  auto opts = manual(clock);
  opts.force_sim = false;
  auto unsimulated = ExtractionLine::create(system_config(real.c_str()), canvas_model(canvas), opts);
  ASSERT_TRUE(unsimulated) << unsimulated.error().what;
  EXPECT_EQ((*unsimulated)->sim(), nullptr);
}

// Two valves on a tee with a volume are both on that volume: the tee is the
// volume's, and no pipe is made between them.
TEST(ExtractionLine, ValvesOnATeeWithAVolumeShareThatVolume) {
  ManualClock clock;
  const char* canvas = R"(
[[valve]]
name = "V1"
pos = [0, 0]

[[valve]]
name = "V2"
pos = [0, 0]

[[stage]]
name = "tank"
pos = [0, 0]
volume = 10.0

[[stage]]
name = "mid"
pos = [0, 0]
volume = 5.0

[[stage]]
name = "line"
pos = [0, 0]
volume = 10.0

[[connection]]
start = "tank"
end = "V1"

[[tee]]
left = "V1"
right = "V2"
mid = "mid"

[[connection]]
start = "V2"
end = "line"
)";
  auto opts = manual(clock);
  opts.sim.outgassing = 0.0;
  opts.sim.outgassing_active = 0.0;
  opts.sim.initial_pressures = {{"tank", 1e-4}};
  auto made = ExtractionLine::create(system_config(kTwoValves), canvas_model(canvas), opts);
  ASSERT_TRUE(made) << made.error().what;
  sim::SimSystem& lab = *(*made)->sim();
  EXPECT_FALSE(lab.has_volume("V1~V2"));
  EXPECT_TRUE(lab.valves_without_physics().empty());
  lab.set_valve("V1", true);
  lab.set_valve("V2", true);
  clock.advance(10s);
  const double level = (10 * 1e-4 + 5 * 1e-8 + 10 * 1e-8) / 25;
  EXPECT_NEAR(*lab.pressure("line"), level, level * 1e-9);
  EXPECT_NEAR(*lab.pressure("mid"), level, level * 1e-9);
}

// Valves the simulator still cannot give physics (joined to one volume, to
// none, or to three or more) are said once, in one line, and not as a
// warning: a drawing may leave a valve's far side off the page.
TEST(ExtractionLine, ValvesWithoutPhysicsAreReportedOnce) {
  ManualClock clock;
  // B dangles from right; kCanvas's M1 is joined to nothing.
  const std::string canvas = std::string(kCanvas) + "\n[[valve]]\nname = \"B\"\npos = [0, 0]\n"
                                                    "\n[[connection]]\nstart = \"right\"\nend = \"B\"\n";
  BuildLog log(clock);
  auto opts = manual(clock);
  opts.log_hub = log.hub;
  auto line = ExtractionLine::create(system_config(), canvas_model(canvas.c_str()), opts);
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_TRUE(log.warnings.empty()) << log.warnings[0];
  ASSERT_EQ(log.no_gas.size(), 1u);
  EXPECT_EQ(log.no_gas[0].level, LogLevel::Info);
  const std::string& said = log.no_gas[0].message;
  EXPECT_EQ(said.find("sim: 2 valve(s) carry no gas in the simulation: B ("), 0u) << said;
  EXPECT_NE(said.find("B (it is joined to one volume only ('right'))"), std::string::npos) << said;
  EXPECT_NE(said.find(", M1 (it is joined to no volume)"), std::string::npos) << said;
  EXPECT_EQ(said.find("A ("), std::string::npos) << said;
  // Each is still a valve, with a state.
  ASSERT_TRUE((*line)->start());
  ASSERT_TRUE((*line)->actuate("B", SwitchOp::Open, "test"));
  EXPECT_TRUE((*line)->sim()->valve_open("B"));

  // A valve joined to a valve and to two volumes is on three: said too.
  //   left --A-- right, and A --B-- far: A has left, right and the pipe A~B.
  BuildLog three(clock);
  opts.log_hub = three.hub;
  const std::string tee = std::string(kCanvas) + "\n[[valve]]\nname = \"B\"\npos = [0, 0]\n"
                                                 "\n[[stage]]\nname = \"far\"\npos = [0, 0]\n"
                                                 "\n[[connection]]\nstart = \"A\"\nend = \"B\"\n"
                                                 "\n[[connection]]\nstart = \"B\"\nend = \"far\"\n";
  ASSERT_TRUE(ExtractionLine::create(system_config(), canvas_model(tee.c_str()), opts));
  ASSERT_EQ(three.no_gas.size(), 1u);
  EXPECT_EQ(three.no_gas[0].message.find("sim: 2 valve(s) carry no gas in the simulation: A (it is joined to 3 volumes"),
            0u)
      << three.no_gas[0].message;
  EXPECT_TRUE(three.warnings.empty());

  // A sound canvas says nothing.
  BuildLog quiet(clock);
  opts.log_hub = quiet.hub;
  const std::string sound = std::string(kCanvas) + "\n[[connection]]\nstart = \"right\"\nend = \"M1\"\n"
                                                   "\n[[stage]]\nname = \"far\"\npos = [0, 0]\n"
                                                   "\n[[connection]]\nstart = \"M1\"\nend = \"far\"\n";
  ASSERT_TRUE(ExtractionLine::create(system_config(), canvas_model(sound.c_str()), opts));
  EXPECT_TRUE(quiet.warnings.empty()) << quiet.warnings[0];
  EXPECT_TRUE(quiet.no_gas.empty()) << quiet.no_gas[0].message;
}

// The NMGRL valve box draws pipe from valve to valve in five places and has
// two tees of two valves and a volume. Read from its canvas, the valves that
// still carry no gas are the ones drawn with one side or neither:
//   FE, FF, FG                          joined to nothing (the furnace's shutters)
//   G, NP-10CRough, GP50Manual_Rough    one side (CO2, NP-10C, GP502): the roughing side is not drawn
//   RDiode                              one side (the Diode tee): likewise
TEST(ExtractionLine, TheNmgrlExampleModelsItsValveToValveJoins) {
  const std::filesystem::path dir = std::filesystem::path(PYCHRON_EXAMPLE_CONFIGS_DIR) / "nmgrl";
  ManualClock clock;
  BuildLog log(clock);
  ExtractionLine::Options opts;
  opts.clock = &clock;
  opts.scheduler.threads = 0;
  opts.run_scheduler = false;
  opts.log_hub = log.hub;
  opts.sim.noise = 0.0;
  opts.state_file = std::filesystem::temp_directory_path() / "pychron-test-nmgrl-pipes.state.toml";
  std::filesystem::remove(opts.state_file);
  auto loaded = ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", opts);
  ASSERT_TRUE(loaded) << loaded.error().what;
  sim::SimSystem& lab = *(*loaded)->sim();

  std::vector<std::string> without;
  for (const auto& [valve, why] : lab.valves_without_physics()) without.push_back(valve);
  const std::vector<std::string> expected{"FE", "FF", "FG", "G", "GP50Manual_Rough", "NP-10CRough", "RDiode"};
  EXPECT_EQ(without, expected);
  ASSERT_EQ(log.no_gas.size(), 1u);
  EXPECT_EQ(log.no_gas[0].level, LogLevel::Info);
  EXPECT_EQ(log.no_gas[0].message.find("sim: 7 valve(s) carry no gas in the simulation: FE ("), 0u)
      << log.no_gas[0].message;
  EXPECT_EQ(log.count("carr"), 0) << "no warning of a valve";

  // The pipes: one for each pair joined by a connection, none for a tee's pair.
  for (const char* pipe : {"ADiode~B", "D~F", "E~I", "FA~FC", "R~S"}) EXPECT_TRUE(lab.has_volume(pipe)) << pipe;
  for (const char* none : {"ADiode~RDiode", "MSManual~R", "B~ADiode", "F~D", "I~E", "FC~FA", "S~R"}) {
    EXPECT_FALSE(lab.has_volume(none)) << none;
  }

  // And gas goes through them. Bone to Minibone is E, the pipe, I.
  ASSERT_TRUE(lab.set_pressure("Bone", 1e-3));
  const double minibone = *lab.pressure("Minibone");
  lab.set_valve("E", true);
  clock.advance(60s);
  EXPECT_GT(*lab.pressure("E~I"), 0.9e-3);
  EXPECT_LT(*lab.pressure("Minibone"), 2 * minibone + 1e-7) << "I is closed";
  lab.set_valve("I", true);
  clock.advance(60s);
  EXPECT_GT(*lab.pressure("Minibone"), 1e-4);
  // Microbone to Jan's getter and Jan is S, the pipe, R, then the tee's
  // volume and MSManual.
  lab.set_valve("E", false);
  lab.set_valve("I", false);
  ASSERT_TRUE(lab.set_pressure("Microbone", 1e-3));
  for (const char* valve : {"S", "R", "MSManual"}) lab.set_valve(valve, true);
  clock.advance(60s);
  EXPECT_GT((*lab.partial_pressures("Jan"))[sim::index(sim::Species::Ar40)], 1e-7);
  // Bone to the diode laser's chamber is B, the pipe, ADiode; to the CO2
  // chamber D, the pipe, F; the furnace manifold to its turbo FC, the pipe, FA.
  ASSERT_TRUE(lab.set_pressure("Bone", 1e-3));
  for (const char* valve : {"B", "ADiode", "D", "F"}) lab.set_valve(valve, true);
  clock.advance(60s);
  EXPECT_GT(*lab.pressure("Diode"), 1e-4);
  EXPECT_GT(*lab.pressure("CO2"), 1e-4);
  ASSERT_TRUE(lab.set_pressure("FurnaceManifold", 1e-3));
  lab.set_valve("FC", true);
  clock.advance(60s);
  EXPECT_GT(*lab.pressure("FA~FC"), 0.5e-3);
  lab.set_valve("FA", true);
  clock.advance(600s);
  EXPECT_LT(*lab.pressure("FurnaceManifold"), 1e-5) << "pumped by FATurbo through FA";
}

TEST(ExtractionLine, WarnsOnceOfASecondSpectrometerStage) {
  ManualClock clock;
  const std::string canvas = std::string(kCanvas) +
                             "\n[[connection]]\nstart = \"right\"\nend = \"M1\"\n"
                             "\n[[stage]]\nname = \"zeta\"\npos = [0, 0]\nkind = \"spectrometer\"\n"
                             "\n[[stage]]\nname = \"argus\"\npos = [0, 0]\nkind = \"spectrometer\"\n"
                             "\n[[connection]]\nstart = \"M1\"\nend = \"argus\"\n";
  BuildLog log(clock);
  auto opts = manual(clock);
  opts.log_hub = log.hub;
  auto line = ExtractionLine::create(system_config(), canvas_model(canvas.c_str()), opts);
  ASSERT_TRUE(line) << line.error().what;
  ASSERT_TRUE((*line)->sim()->spectrometer_volume());
  EXPECT_EQ(*(*line)->sim()->spectrometer_volume(), "argus");
  ASSERT_EQ(log.warnings.size(), 1u);
  EXPECT_NE(log.warnings[0].find("'zeta' is a second spectrometer"), std::string::npos) << log.warnings[0];
  EXPECT_NE(log.warnings[0].find("'argus'"), std::string::npos) << log.warnings[0];
}

// The example lab: each stage is to the gas what the canvas says it is,
// with its sim.toml or with no file at all. The file changes numbers only:
// the air in the tank, what a pumped volume holds, the inlet, the walls.
void expect_the_example_lab(bool with_sim_toml) {
  const std::filesystem::path dir = PYCHRON_EXAMPLE_CONFIGS_DIR;
  ASSERT_TRUE(std::filesystem::is_regular_file(dir / "sim.toml"));
  ManualClock clock;
  BuildLog log(clock);
  ExtractionLine::Options opts;
  opts.clock = &clock;
  opts.scheduler.threads = 0;
  opts.run_scheduler = false;
  opts.log_hub = log.hub;
  if (!with_sim_toml) opts.sim_file = std::filesystem::path{};
  opts.state_file = std::filesystem::temp_directory_path() / "pychron-test-example-sim-toml.state.toml";
  std::filesystem::remove(opts.state_file);
  auto loaded = ExtractionLine::load(dir / "extraction_line.toml", dir / "canvas.toml", opts);
  ASSERT_TRUE(loaded) << loaded.error().what;
  auto& line = **loaded;
  sim::SimSystem& lab = *line.sim();
  EXPECT_TRUE(log.warnings.empty()) << log.warnings[0];
  EXPECT_FALSE(lab.build_error());
  EXPECT_TRUE(lab.valves_without_physics().empty());
  const std::size_t ar40 = sim::index(sim::Species::Ar40);
  const std::size_t ar36 = sim::index(sim::Species::Ar36);
  const std::size_t active = sim::index(sim::Species::Active);

  // The spectrometer stage is the source.
  ASSERT_TRUE(lab.spectrometer_volume());
  EXPECT_EQ(*lab.spectrometer_volume(), "spec");

  // What sim.toml sets, and what the simulator has unasked.
  const sim::SimSettings defaults;
  const sim::SimSettings& set = lab.settings();
  const double tank_ar40 = with_sim_toml ? 9.5e-5 : defaults.tank_argon40;
  const double pumped = with_sim_toml ? 1e-10 : defaults.default_pressure;
  EXPECT_EQ(set.default_pressure, pumped);
  EXPECT_EQ(set.outgassing, with_sim_toml ? 1.5e-13 : defaults.outgassing);
  if (with_sim_toml) {
    EXPECT_EQ(set.conductances, (std::map<std::string, double>{{"B", 0.02}}));
    ASSERT_TRUE(set.pumps.contains("turbo"));
    EXPECT_EQ(set.pumps.at("turbo").base, 1e-10);
    EXPECT_EQ(set.pump_speeds.at("turbo"), defaults.pump_speed);
  } else {
    EXPECT_TRUE(set.conductances.empty());
    EXPECT_TRUE(set.pumps.empty());
  }
  EXPECT_EQ(set.source.consumption, defaults.source.consumption);
  EXPECT_EQ(set.source.sensitivity, defaults.source.sensitivity);
  EXPECT_EQ(set.source.memory_fa_per_s, defaults.source.memory_fa_per_s);
  EXPECT_TRUE(set.sizes.empty());

  // The tank starts with air, and nothing else does.
  const sim::Composition tank = *lab.partial_pressures("air_tank");
  EXPECT_EQ(tank, sim::with_ar40(sim::air_ratios(), tank_ar40));
  EXPECT_DOUBLE_EQ(tank[ar40] / tank[ar36], 298.56);
  EXPECT_GT(tank[active], 100 * tank[ar40]);
  EXPECT_NEAR(*lab.pressure("prep"), pumped, pumped * 1e-4);

  // The pipette is a volume between two valves, the tank on one side and
  // the line on the other.
  ASSERT_NE(line.network(), nullptr);
  EXPECT_EQ(line.network()->neighbors("air"), (std::set<std::string>{"P1", "P2"}));
  EXPECT_TRUE(line.network()->is_valve("P1"));
  EXPECT_TRUE(line.network()->is_valve("P2"));
  EXPECT_TRUE(line.network()->neighbors("P2").contains("air_tank"));
  EXPECT_TRUE(line.network()->neighbors("P1").contains("prep"));
  ASSERT_TRUE(lab.has_volume("air"));
  // It is 0.1 cc: opened to the tank's 50 it takes the tank's gas and the
  // tank barely notices; then its shot into prep's 50 cc is a five-hundredth.
  ASSERT_TRUE(lab.set_composition("air", sim::Composition{}));
  ASSERT_TRUE(lab.set_composition("prep", sim::Composition{}));
  lab.set_valve("P2", true);
  clock.advance(10s);
  lab.set_valve("P2", false);
  const double loaded_ar40 = (*lab.partial_pressures("air"))[ar40];
  EXPECT_NEAR(loaded_ar40, tank_ar40 * 50.0 / 50.1, tank_ar40 * 1e-6);
  lab.set_valve("P1", true);
  clock.advance(100ms);  // a hundred of the valve's time constants; outgassing has added 5e-14
  EXPECT_NEAR((*lab.partial_pressures("prep"))[ar40], loaded_ar40 * 0.1 / 50.1, loaded_ar40 * 0.1 / 50.1 * 1e-4);
  lab.set_valve("P1", false);

  // Pumped volumes: the turbo (with the two gauges drawn on its pipe) and
  // the roughing pump come down by themselves; prep, valved off, does not.
  ASSERT_TRUE(lab.set_pressure("turbo", 1e-3));
  ASSERT_TRUE(lab.set_pressure("rough", 1e-3));
  ASSERT_TRUE(lab.set_pressure("prep", 1e-3));
  clock.advance(1s);
  EXPECT_LT(*lab.pressure("turbo"), 1e-8);
  EXPECT_LT(*lab.pressure("IG1"), 1e-8);
  EXPECT_LT(*lab.pressure("rough"), 1e-8);
  EXPECT_GT(*lab.pressure("prep"), 0.99e-3);
  // And prep through C.
  lab.set_valve("C", true);
  clock.advance(60s);
  EXPECT_LT(*lab.pressure("prep"), 1e-7);
}

TEST(ExtractionLine, TheExampleLabLoadsWithItsSimToml) { expect_the_example_lab(true); }

// The roles are the canvas's, not the file's.
TEST(ExtractionLine, TheExampleLabIsTheSameLabWithNoSimToml) { expect_the_example_lab(false); }

}  // namespace
