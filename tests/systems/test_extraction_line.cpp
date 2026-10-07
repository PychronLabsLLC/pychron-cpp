#include "pychron/systems/extraction_line.hpp"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
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

// Every warning the line logs while it is built, through a hub of the test's.
struct BuildLog {
  explicit BuildLog(const Clock& clock) {
    sub = bus.subscribe<Log>([this](const Log& e) {
      if (e.logger == "extraction_line" && e.level == LogLevel::Warn) warnings.push_back(e.message);
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
  SignalBus::Subscription sub;
  std::shared_ptr<LogHub> hub;
};

TEST(ExtractionLine, WarnsOnceOfEachValveTheSimulatorCannotModel) {
  ManualClock clock;
  // right --B-- M1 (two valves and no volume between), on top of the canvas.
  const std::string canvas = std::string(kCanvas) +
                             "\n[[valve]]\nname = \"B\"\npos = [0, 0]\n"
                             "\n[[connection]]\nstart = \"right\"\nend = \"B\"\n"
                             "\n[[connection]]\nstart = \"B\"\nend = \"M1\"\n";
  BuildLog log(clock);
  auto opts = manual(clock);
  opts.log_hub = log.hub;
  auto line = ExtractionLine::create(system_config(), canvas_model(canvas.c_str()), opts);
  ASSERT_TRUE(line) << line.error().what;
  EXPECT_EQ(log.count("valve 'B' carries no gas in the simulation"), 1) << "B: joined to M1";
  EXPECT_EQ(log.count("valve 'M1' carries no gas in the simulation"), 1) << "M1: joined to B and to nothing else";
  EXPECT_EQ(log.count("valve 'A' carries"), 0);
  ASSERT_EQ(log.warnings.size(), 2u);
  EXPECT_NE(log.warnings[0].find("valve 'M1' with no volume between"), std::string::npos) << log.warnings[0];
  // It is still a valve, with a state.
  ASSERT_TRUE((*line)->start());
  ASSERT_TRUE((*line)->actuate("B", SwitchOp::Open, "test"));
  EXPECT_TRUE((*line)->sim()->valve_open("B"));

  // A sound canvas warns of nothing. (kCanvas's M1 is joined to nothing.)
  BuildLog quiet(clock);
  opts.log_hub = quiet.hub;
  const std::string sound = std::string(kCanvas) + "\n[[connection]]\nstart = \"right\"\nend = \"M1\"\n"
                                                   "\n[[stage]]\nname = \"far\"\npos = [0, 0]\n"
                                                   "\n[[connection]]\nstart = \"M1\"\nend = \"far\"\n";
  ASSERT_TRUE(ExtractionLine::create(system_config(), canvas_model(sound.c_str()), opts));
  EXPECT_TRUE(quiet.warnings.empty()) << quiet.warnings[0];
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

// The example lab: each stage is to the gas what the canvas says it is.
// With its sim.toml, which repeats the defaults, or with no file at all.
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

  // The tank starts with air, and nothing else does.
  const sim::Composition tank = *lab.partial_pressures("air_tank");
  EXPECT_EQ(tank, sim::with_ar40(sim::air_ratios(), 3e-5));
  EXPECT_DOUBLE_EQ(tank[ar40] / tank[ar36], 298.56);
  EXPECT_GT(tank[active], 100 * tank[ar40]);
  EXPECT_NEAR(*lab.pressure("prep"), 1e-8, 1e-12);

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
  EXPECT_NEAR(loaded_ar40, 3e-5 * 50.0 / 50.1, 3e-5 * 1e-6);
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
