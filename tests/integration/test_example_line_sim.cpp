// The M1 success criterion under simulation: the example extraction line
// (every transport kind = "sim") runs from extraction_line.toml + canvas.toml
// through the same facade, drivers and managers as hardware. Valves actuate
// with interlocks enforced and gauges scan, with SimSystem standing in for
// the physical lab.
//
// Example plumbing (canvas.toml):
//   bone --A-- prep --B-- spec
//               |   |
//              P1   C --+-- turbo --+-- M1 -- rough
//                       |           |
//                      IG1         PG1
// A and C interlock each other; IG1 has alarm_high = 1e-4.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "virtual_time.hpp"

namespace {

using namespace pychron;
using namespace pychron::systems;
using namespace std::chrono_literals;

const std::filesystem::path kDir = PYCHRON_EXAMPLE_CONFIGS_DIR;

sim::SimSettings lab() {
  sim::SimSettings s;
  s.default_pressure = 1e-8;
  s.initial_pressures = {{"bone", 1e-3}};  // gas released into the furnace
  s.pumps = {{"turbo", {1e-9, 5s}}};
  s.noise = 0.0;
  // These tests are about valves, interlocks, scans and alarms, and state
  // what the gas does in round numbers: walls that give nothing off, and
  // valves wide enough that gas is across one before a pump has taken any.
  s.outgassing = 0.0;
  s.outgassing_active = 0.0;
  s.valve_conductance = 1e6;
  return s;
}

// Every captured event, in order, across bus threads.
struct Recorder {
  explicit Recorder(SignalBus& bus) {
    subs.push_back(bus.subscribe<ValveChanged>([this](const ValveChanged& e) { add(valves, e); }));
    subs.push_back(bus.subscribe<ActuationFailed>([this](const ActuationFailed& e) { add(failures, e); }));
    subs.push_back(bus.subscribe<PressureSample>([this](const PressureSample& e) { add(samples, e); }));
    subs.push_back(bus.subscribe<Alarm>([this](const Alarm& e) { add(alarms, e); }));
    subs.push_back(bus.subscribe<Snapshot>([this](const Snapshot& e) { add(snapshots, e); }));
  }

  template <class E>
  void add(std::vector<E>& into, const E& e) {
    std::lock_guard lock(mutex);
    into.push_back(e);
  }

  std::size_t samples_of(const std::string& gauge) {
    std::lock_guard lock(mutex);
    std::size_t n = 0;
    for (const auto& s : samples) n += s.gauge == gauge ? 1 : 0;
    return n;
  }

  std::mutex mutex;
  std::vector<ValveChanged> valves;
  std::vector<ActuationFailed> failures;
  std::vector<PressureSample> samples;
  std::vector<Alarm> alarms;
  std::vector<Snapshot> snapshots;
  std::vector<SignalBus::Subscription> subs;
};

// Deterministic: ManualClock, scans dispatched inline by the test, and the
// configured settle times zeroed so actuations do not wait on the clock.
class ExampleLineSim : public ::testing::Test {
 protected:
  void SetUp() override {
    auto cfg = config::load_system_config(kDir / "extraction_line.toml");
    ASSERT_TRUE(cfg) << cfg.error().what;
    for (auto& v : cfg->valves) v.settle_ms = 0;
    auto canvas = canvas::load_canvas(kDir / "canvas.toml");
    ASSERT_TRUE(canvas) << canvas.error().what;

    ExtractionLine::Options options;
    options.clock = &clock;
    options.scheduler.threads = 0;
    options.run_scheduler = false;
    options.sim = lab();
    auto made = ExtractionLine::create(std::move(*cfg), std::move(*canvas), options);
    ASSERT_TRUE(made) << made.error().what;
    line = std::move(*made);
    events = std::make_unique<Recorder>(line->bus());
    ASSERT_TRUE(line->start());
    // start() reads every switch back (Unknown -> Closed); keep only what
    // the tests themselves cause.
    std::lock_guard lock(events->mutex);
    events->valves.clear();
  }

  void scan_after(Duration d) {
    clock.advance(d);
    line->scheduler().run_pending();
    line->scheduler().wait_idle();
  }

  double latest(const std::string& gauge) {
    std::lock_guard lock(events->mutex);
    for (auto it = events->samples.rbegin(); it != events->samples.rend(); ++it) {
      if (it->gauge == gauge) return it->value;
    }
    ADD_FAILURE() << "no sample for " << gauge;
    return -1;
  }

  ManualClock clock;
  std::unique_ptr<ExtractionLine> line;
  std::unique_ptr<Recorder> events;
};

TEST_F(ExampleLineSim, StartsClosedAndQuiet) {
  const auto snap = line->snapshot();
  for (const char* v : {"A", "B", "C", "P1", "P2", "pump_power"}) {
    EXPECT_EQ(snap.valves.at(v), ValveState::Closed) << v;
  }
  EXPECT_EQ(snap.valves.at("M1"), ValveState::Unknown);
  EXPECT_NEAR(snap.pressures.at("IG1"), 1e-8, 1e-12);
  EXPECT_NEAR(snap.pressures.at("PG1"), 1e-8, 1e-12);

  scan_after(1s);
  EXPECT_EQ(events->samples_of("IG1"), 1u);
  EXPECT_EQ(events->samples_of("PG1"), 1u);
  EXPECT_TRUE(events->alarms.empty());
}

TEST_F(ExampleLineSim, GasFlowsThroughOpenValvesAndInterlocksHold) {
  // Expand furnace gas into prep: bone (12.5 cc) + prep (1 cc).
  ASSERT_TRUE(line->actuate("A", SwitchOp::Open, "test"));
  clock.advance(100us);  // long for the valve, nothing to the pump
  const double expanded = (12.5 * 1e-3 + 1e-8) / 13.5;
  EXPECT_NEAR(*line->sim()->pressure("prep"), expanded, expanded * 1e-9);
  EXPECT_TRUE(line->sim()->valve_open("A"));

  // C would expose the furnace to the turbo: refused, nothing sent.
  auto refused = line->actuate("C", SwitchOp::Open, "test");
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Interlock);
  ASSERT_EQ(events->failures.size(), 1u);
  EXPECT_EQ(events->failures[0].valve, "C");
  EXPECT_FALSE(line->sim()->valve_open("C"));
  // Long enough for an open C to have shown on IG1; the pump has taken a
  // tenth of the tolerance.
  clock.advance(100us);
  EXPECT_NEAR(*line->read_gauge("IG1"), 1e-8, 1e-12);

  // Isolate the furnace, then pump prep through C. The turbo region
  // (prep, turbo, IG1, PG1: 1 cc each) spikes above IG1's alarm_high.
  ASSERT_TRUE(line->actuate("A", SwitchOp::Close, "test"));
  ASSERT_TRUE(line->actuate("C", SwitchOp::Open, "test"));
  clock.advance(100us);
  const double spike = (expanded + 3e-8) / 4;
  // The MaxiGauge wire format carries five significant digits.
  EXPECT_NEAR(*line->read_gauge("IG1"), spike, spike * 1e-4);
  EXPECT_NEAR(*line->read_gauge("PG1"), spike, spike * 1e-4);

  // The pump's 5 s is for turbo's own 1 cc; it has the region's 4 cc to
  // empty, at a time constant of 20 s.
  scan_after(4s);  // a fifth of the pump time constant: still above alarm_high
  const double after_1s = 1e-9 + (spike - 1e-9) * std::exp(-0.2);
  EXPECT_NEAR(latest("IG1"), after_1s, after_1s * 1e-4);
  ASSERT_EQ(events->alarms.size(), 1u);
  EXPECT_EQ(events->alarms[0].source, "IG1");

  scan_after(240s);
  EXPECT_LT(latest("IG1"), 1e-8);
  // Isolated behind A at the expanded pressure.
  EXPECT_NEAR(*line->sim()->pressure("bone"), expanded, expanded * 1e-9);

  std::vector<std::string> changed;
  for (const auto& e : events->valves) changed.push_back(e.valve);
  EXPECT_EQ(changed, (std::vector<std::string>{"A", "A", "C"}));
  const auto snap = line->snapshot();
  EXPECT_EQ(snap.valves.at("A"), ValveState::Closed);
  EXPECT_EQ(snap.valves.at("C"), ValveState::Open);
}

TEST_F(ExampleLineSim, SwitchesShareTheRelayBoard) {
  ASSERT_TRUE(line->actuate("pump_power", SwitchOp::Open, "test"));
  EXPECT_TRUE(line->sim()->valve_open("pump_power"));
  EXPECT_FALSE(line->sim()->valve_open("A"));
}

TEST_F(ExampleLineSim, PipetteValvesNeverOpenTogether) {
  ASSERT_TRUE(line->actuate("P1", SwitchOp::Open, "test"));
  auto second = line->actuate("P2", SwitchOp::Open, "test");
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().kind, ErrorKind::Interlock);
  EXPECT_FALSE(line->sim()->valve_open("P2"));
}

// The scheduler's own threads and the example files exactly as committed
// (including A's 500 ms settle), in simulated time: the test's thread takes
// part in a VirtualClock, and time moves only while it waits there.
using ExampleLineSimThreaded = pychron::testing::VirtualTimeTest;

TEST_F(ExampleLineSimThreaded, ScansAndActuatesOnSchedulerThreads) {
  VirtualClock clock;
  Clock::Participant test(clock, "test");
  ExtractionLine::Options options;
  options.clock = &clock;
  options.sim = lab();
  // Valve states and locks persist beside the config by default: keep the
  // test out of the repo and independent of earlier runs.
  options.state_file = std::filesystem::temp_directory_path() / "pychron-test-example-line.state.toml";
  std::filesystem::remove(options.state_file);
  auto made = ExtractionLine::load(kDir / "extraction_line.toml", kDir / "canvas.toml", options);
  ASSERT_TRUE(made) << made.error().what;
  auto& line = **made;
  Recorder events(line.bus());
  ASSERT_TRUE(line.start());
  ASSERT_EQ(events.snapshots.size(), 1u);

  const TimePoint before = clock.now();
  ASSERT_TRUE(line.actuate("A", SwitchOp::Open, "test"));
  EXPECT_EQ(line.snapshot().valves.at("A"), ValveState::Open);
  EXPECT_GE(clock.now() - before, 500ms) << "A's settle is waited for";

  // The gauges are scanned once a second (scan_interval_ms), on a worker.
  const auto scanned = events.samples_of("IG1");
  clock.sleep_for(3s);
  EXPECT_GE(events.samples_of("IG1"), scanned + 2) << "IG1 was not scanned in 3 s";
  EXPECT_LE(events.samples_of("IG1"), scanned + 4);

  line.stop();
  EXPECT_FALSE(line.running());
  const auto n = events.samples_of("IG1");
  clock.sleep_for(1200ms);
  EXPECT_EQ(events.samples_of("IG1"), n);
}

// stop() keeps the line's start-and-stop mutex while it waits for a job under
// way, and the job takes clock time. Another thread that asks whether the
// line is running waits for that mutex through the clock: blocked any other
// way it looks runnable, time stands, and the job never ends.
TEST_F(ExampleLineSimThreaded, AskingWhetherItRunsDuringAStopDoesNotStallTime) {
  VirtualClock clock;
  Clock::Participant test(clock, "test");
  std::atomic<int> slow_runs{0};  // the line's job counts here: declared before the line, so it outlives it
  ExtractionLine::Options options;
  options.clock = &clock;
  options.sim = lab();
  options.state_file = std::filesystem::temp_directory_path() / "pychron-test-example-line-stop.state.toml";
  std::filesystem::remove(options.state_file);
  auto made = ExtractionLine::load(kDir / "extraction_line.toml", kDir / "canvas.toml", options);
  ASSERT_TRUE(made) << made.error().what;
  auto& line = **made;
  ASSERT_TRUE(line.start());

  // A job that takes ten seconds of the clock's time, once.
  const TimePoint start = clock.now();
  ASSERT_TRUE(line.scheduler().every("slow", 1s, [&] {
    if (slow_runs.fetch_add(1) == 0) clock.sleep_for(10s);
  }));
  clock.sleep_for(1500ms);
  ASSERT_EQ(slow_runs.load(), 1) << "the job is under way, asleep until start + 11 s";

  TimePoint stopped{};
  pychron::testing::Crew crew(clock);
  crew.start("stopper", [&] {
    line.stop();
    stopped = clock.now();
  });
  // stop() has the mutex by the time it has stopped the dispatcher, and keeps
  // it while it waits for the job. This thread is runnable, so time stands.
  ASSERT_TRUE(pychron::testing::eventually_real([&] { return !line.scheduler().started(); }));
  EXPECT_FALSE(line.running());
  EXPECT_EQ(clock.now(), start + 11s) << "the asker waited, in the clock, for the stop to finish";
  crew.join();
  EXPECT_EQ(stopped, start + 11s);
}

// On hardware the clock is a SteadyClock and nobody is a participant: the
// same files, the scheduler's own threads, real time.
TEST(ExampleLineSteady, ActuatesAndScansInRealTime) {
  ExtractionLine::Options options;  // no clock given: the line's own SteadyClock
  options.sim = lab();
  options.state_file = std::filesystem::temp_directory_path() / "pychron-test-example-line-steady.state.toml";
  std::filesystem::remove(options.state_file);
  auto made = ExtractionLine::load(kDir / "extraction_line.toml", kDir / "canvas.toml", options);
  ASSERT_TRUE(made) << made.error().what;
  auto& line = **made;
  Recorder events(line.bus());
  ASSERT_TRUE(line.start());
  EXPECT_TRUE(line.running());

  const auto before = std::chrono::steady_clock::now();
  ASSERT_TRUE(line.actuate("A", SwitchOp::Open, "test"));
  EXPECT_EQ(line.snapshot().valves.at("A"), ValveState::Open);
  EXPECT_GE(std::chrono::steady_clock::now() - before, 500ms) << "A's settle is waited for, in real time";

  // The gauges are scanned once a second, on a worker.
  const auto scanned = events.samples_of("IG1");
  EXPECT_TRUE(pychron::testing::eventually_real([&] { return events.samples_of("IG1") > scanned; }, 10s))
      << "IG1 was not scanned in 10 s";
  const auto pressure = line.read_gauge("IG1");
  ASSERT_TRUE(pressure) << pressure.error().what;
  EXPECT_GT(*pressure, 0.0);

  line.stop();
  EXPECT_FALSE(line.running());
  const auto n = events.samples_of("IG1");
  std::this_thread::sleep_for(1200ms);  // more than a scan interval: nothing scans a stopped line
  EXPECT_EQ(events.samples_of("IG1"), n);
}

}  // namespace
