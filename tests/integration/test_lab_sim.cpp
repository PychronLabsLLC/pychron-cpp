// The example lab's physics, end to end (lab simulator spec section 8): a
// scratch copy of configs/examples run on a VirtualClock at unlimited speed
// through LabSession, with the example's own scripts, queue
// (experiment.sim-air.toml), measurement plan and sim.toml. What a run
// measured is read back from its saved record.
//
// What is expected is worked out from the lab's own numbers (the simulator's
// settings as the line loaded them: sizes, the inlet's conductance, what the
// source uses, the tank and the pipette), not from constants copied here. A
// measured value is recorded as a test property beside what it was held to
// (`--gtest_output=json` shows them).
//
// The test's thread takes part in the clock: time moves only while it waits
// there (session.wait()).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/events.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/lab/lab.hpp"
#include "pychron/experiment/lab/session.hpp"
#include "pychron/experiment/model/queue_file.hpp"
#include "pychron/experiment/model/run_spec.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/experiment/record/types.hpp"
#include "pychron/experiment/run/run.hpp"
#include "pychron/reduction/fits.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"
#include "virtual_time.hpp"

namespace pychron::experiment::lab {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// The example lab's names: extraction_line.toml and canvas.toml.
constexpr const char* kInlet = "B";   // [aliases] valves.inlet: prep to the spectrometer
constexpr const char* kOutlet = "C";  // [aliases] valves.outlet: prep to the turbo
constexpr const char* kPrep = "prep";
constexpr const char* kSource = "spec";
constexpr const char* kTank = "air_tank";
constexpr const char* kPipette = "air";
constexpr const char* kFaraday = "H1";  // Ar40, by the plan
constexpr const char* kCounter = "CDD";  // Ar36, by the plan

constexpr std::size_t kAr40 = sim::index(sim::Species::Ar40);
constexpr std::size_t kAr36 = sim::index(sim::Species::Ar36);

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

// One run as it was saved.
struct Analysis {
  executor::RunSummary summary;
  record::AnalysisRecord record;
};

// A scratch copy of the example lab, simulated, on its own clock: the line,
// the spectrometer whose beam reads the line's gas, and a session. Built
// from nothing each time, as an application builds it.
class ScratchLab {
 public:
  // `prepare` changes the copy before anything is loaded from it.
  explicit ScratchLab(const std::function<void(const fs::path&)>& prepare = {}) {
    dir_ = fs::temp_directory_path() /
           ("pychron-labsim-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(std::random_device{}()));
    fs::copy(fs::path(PYCHRON_EXAMPLE_CONFIGS_DIR), dir_, fs::copy_options::recursive);
    fs::remove_all(dir_ / "data");
    if (prepare) prepare(dir_);

    systems::ExtractionLine::Options options;
    options.clock = &clock_;
    options.force_sim = true;
    options.state_file = dir_ / "line.state.toml";
    auto line = systems::ExtractionLine::load(dir_ / "extraction_line.toml", dir_ / "canvas.toml", options);
    if (!line) {
      problem_ = line.error().what;
      return;
    }
    line_ = std::move(*line);
    if (auto started = line_->start(); !started) {
      problem_ = started.error().what;
      return;
    }
    if (line_->sim() == nullptr) {
      problem_ = "the line is not simulated";
      return;
    }
    auto spec = spectrometer::load_spectrometer_for_app(
        dir_ / "spectrometer.sim-integrated.toml",
        spectrometer::SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
        // As the applications do: the beam measures the line's gas.
        spectrometer::SpectrometerBringup{.sim_beam_from_table = true, .line_sim = line_->sim()});
    if (!spec) {
      problem_ = spec.error().what;
      return;
    }
    spec_ = std::move(*spec);
    lab_ = load_lab({dir_, dir_ / "extraction_line.toml", dir_ / "spectrometer.sim-integrated.toml"});
    if (!lab_.problems.empty()) {
      problem_ = lab_.problems.front();
      return;
    }
    session_ = std::make_unique<LabSession>(lab_, SessionHardware{*line_, spec_.get(), nullptr, {}},
                                            SessionOptions{dir_ / "data", {}, {}});
    start_ = clock_.now();
    valves_sub_ = line_->bus().subscribe<ValveChanged>([this](const ValveChanged& e) {
      std::lock_guard lock(mutex_);
      valve_events_.push_back(e);
    });
  }

  ~ScratchLab() {
    valves_sub_ = {};
    session_.reset();
    if (line_) line_->stop();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
    line_.reset();
    fs::remove_all(dir_);
  }
  ScratchLab(const ScratchLab&) = delete;
  ScratchLab& operator=(const ScratchLab&) = delete;

  // Why the lab could not be built; empty when it was.
  const std::string& problem() const { return problem_; }
  const fs::path& dir() const { return dir_; }
  VirtualClock& clock() { return clock_; }
  sim::SimSystem& sim() { return *line_->sim(); }
  const sim::SimSettings& settings() { return line_->sim()->settings(); }
  // The beam the simulated spectrometer reads (the bringup registers it).
  std::shared_ptr<sim::BeamModel> beam() { return sim::BeamModelRegistry::global().acquire("default", clock_); }
  // Seconds of the clock since the lab was built.
  double since_start(TimePoint t) const { return seconds(t - start_); }

  // The example's air queue: blank, air, air, air, blank.
  QueueSpec air_queue() {
    auto q = load_queue_file((dir_ / "experiment.sim-air.toml").string(), lab_.ids);
    if (!q) {
      ADD_FAILURE() << q.error().what;
      return {};
    }
    return *q;
  }
  // The same queue with only these rows of it, in this order.
  QueueSpec rows(std::initializer_list<std::size_t> which) {
    const QueueSpec all = air_queue();
    QueueSpec q = all;
    q.runs.clear();
    for (const std::size_t row : which) {
      if (row < all.runs.size()) {
        q.runs.push_back(all.runs[row]);
      } else {
        ADD_FAILURE() << "the air queue has no row " << row;
      }
    }
    return q;
  }

  // Runs the queue to its end and reads back what each run saved. Every
  // run must have succeeded; fewer analyses than runs is a failure reported
  // here.
  std::vector<Analysis> run(const QueueSpec& queue) {
    std::vector<Analysis> out;
    if (auto started = session_->start(queue); !started) {
      ADD_FAILURE() << started.error().what;
      return out;
    }
    const auto result = session_->wait();
    if (!result) {
      ADD_FAILURE() << "the session ran no queue";
      return out;
    }
    EXPECT_EQ(result->end, executor::QueueEnd::Completed) << result->reason;
    EXPECT_EQ(result->runs.size(), queue.runs.size());
    for (const auto& summary : result->runs) {
      EXPECT_EQ(summary.state, run::RunState::Success) << summary.run_id << ": " << summary.error.value_or("");
      const fs::path file = dir_ / "data" / "records" / summary.identifier /
                            (summary.identifier + "-" + std::to_string(summary.aliquot) + ".json");
      std::ifstream in(file);
      if (!in) {
        ADD_FAILURE() << "no record " << file;
        continue;
      }
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      auto record = record::from_json(text);
      if (!record) {
        ADD_FAILURE() << file << ": " << record.error().what;
        continue;
      }
      out.push_back({summary, std::move(*record)});
    }
    return out;
  }

  // When `valve` was told to go to `state`, in order.
  std::vector<TimePoint> valve_times(std::string_view valve, ValveState state) {
    std::lock_guard lock(mutex_);
    std::vector<TimePoint> out;
    for (const auto& e : valve_events_) {
      if (e.valve == valve && e.state == state) out.push_back(e.ts);
    }
    return out;
  }

 private:
  fs::path dir_;
  VirtualClock clock_;  // before everything that is given a reference to it
  Clock::Participant test_{clock_, "test"};
  std::string problem_;
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
  Lab lab_;
  std::unique_ptr<LabSession> session_;
  TimePoint start_{};
  std::mutex mutex_;
  std::vector<ValveChanged> valve_events_;
  SignalBus::Subscription valves_sub_;
};

// --- reading a record -------------------------------------------------------

// The series of `kind` for an isotope (a baseline has none: give the
// detector); null when the record has none.
const record::Trace* trace_of(const record::AnalysisRecord& rec, std::string_view kind, std::string_view isotope,
                              std::string_view detector) {
  for (const auto& s : rec.data.series) {
    if (s.kind == kind && s.iso == isotope && s.det == detector) return &s.trace;
  }
  return nullptr;
}

reduction::Series series_of(const record::Trace& trace) {
  reduction::Series s;
  s.x.assign(trace.t.begin(), trace.t.end());
  s.y.assign(trace.v.begin(), trace.v.end());
  return s;
}

// A straight line through a series: its value at time zero, with the
// standard error of that, and its slope with its own.
struct Line {
  double intercept = 0, intercept_error = 0;
  double slope = 0, slope_error = 0;
  double mean = 0;  // of the readings
  std::size_t n = 0;
};

std::optional<Line> line_through(const reduction::Series& series) {
  const auto fitted = reduction::fit(series, {.kind = reduction::FitKind::Linear, .error = reduction::ErrorType::Sem});
  if (!fitted || fitted->params.size() != 2) return std::nullopt;
  Line out;
  out.intercept = fitted->value;
  out.intercept_error = fitted->error;
  out.slope = fitted->params[1];
  out.n = series.x.size();
  double mean_x = 0;
  for (std::size_t i = 0; i < out.n; ++i) {
    mean_x += series.x[i];
    out.mean += series.y[i];
  }
  mean_x /= static_cast<double>(out.n);
  out.mean /= static_cast<double>(out.n);
  double spread = 0;
  for (const double x : series.x) spread += (x - mean_x) * (x - mean_x);
  out.slope_error = fitted->residual_sd / std::sqrt(spread);
  return out;
}

// What a run measured of one isotope: the line through its signal, and the
// mean of the run's own baseline on that detector.
struct Measured {
  Line signal;
  double baseline = 0;
  // The time-zero intercept less the baseline, and its standard error.
  double value() const { return signal.intercept - baseline; }
  double error() const { return signal.intercept_error; }
};

std::optional<Measured> measured(const record::AnalysisRecord& rec, std::string_view isotope,
                                 std::string_view detector) {
  const auto* signal = trace_of(rec, "signal", isotope, detector);
  const auto* baseline = trace_of(rec, "baseline", "", detector);
  if (signal == nullptr || baseline == nullptr) return std::nullopt;
  const auto line = line_through(series_of(*signal));
  const auto mean = reduction::fit(series_of(*baseline), {.kind = reduction::FitKind::Average});
  if (!line || !mean) return std::nullopt;
  return Measured{*line, mean->value};
}

// --- the lab's numbers ------------------------------------------------------

// A volume's size, litres: sim.toml's for the name, else `unsized_cc` (what
// its kind of stage is given when the canvas sizes it no more than the
// example's does).
double litres(const sim::SimSettings& s, const std::string& name, double unsized_cc) {
  const auto own = s.sizes.find(name);
  return (own == s.sizes.end() ? unsized_cc : own->second) / 1000.0;
}

struct LabNumbers {
  double prep = 0, source = 0, tank = 0, pipette = 0;  // litres
  double inlet = 0;        // L/s for Ar40
  double sensitivity = 0;  // fA per mbar
  double consumption = 0;  // 1/s
  double rise = 0;         // fA/s of Ar40 in the shut source: its walls and its memory
  double pumped = 0;       // fA of Ar40 in a volume as the lab starts
  double tank_ar40 = 0;    // mbar

  // The inlet's time constant between prep and the source, s.
  double inlet_tau() const { return prep * source / ((prep + source) * inlet); }
  // What a shot leaves of the tank's pressure: V_tank / (V_tank + V_pipette).
  double depletion() const { return tank / (tank + pipette); }
  // The Ar40 of the first shot once it has spread over prep and the source,
  // fA: the pipette filled from the tank, emptied into prep, and prep's
  // share let through the inlet.
  double first_shot() const {
    const double in_pipette = tank_ar40 * depletion();
    const double in_prep = in_pipette * pipette / (pipette + prep);
    return in_prep * prep / (prep + source) * sensitivity;
  }
};

LabNumbers numbers_of(const sim::SimSettings& s) {
  LabNumbers n;
  n.prep = litres(s, kPrep, s.default_volume_cc);
  n.source = litres(s, kSource, s.default_volume_cc);
  n.tank = litres(s, kTank, s.default_volume_cc);
  n.pipette = litres(s, kPipette, s.pipette_cc);
  const auto own = s.conductances.find(kInlet);
  n.inlet = own == s.conductances.end() ? s.valve_conductance : own->second;
  n.sensitivity = s.source.sensitivity;
  n.consumption = s.source.consumption;
  // Per litre of wall, into that litre: mbar/s whatever the size.
  n.rise = s.outgassing * s.source.sensitivity + s.source.memory_fa_per_s;
  n.pumped = s.default_pressure * sim::air_ratios()[kAr40] / sim::total(sim::air_ratios()) * s.source.sensitivity;
  const auto held = s.compositions.find(kTank);
  n.tank_ar40 = held == s.compositions.end() ? s.tank_argon40 : held->second[kAr40];
  return n;
}

// The 1-sigma noise of one Faraday reading of `signal` fA.
double faraday_sigma(const sim::BeamDetector& d, double signal) { return d.noise_floor + d.noise_rel * std::abs(signal); }

// The fraction of the ions a counter at its voltage counts: the plateau of
// the simulated multiplier (BeamModel's `sensitivity_locked`). 1 on a Faraday.
double counter_yield(const sim::BeamDetector& d) {
  const sim::BeamSettings beam;
  return 1.0 / (1.0 + std::exp(-(d.cdd_voltage - beam.cdd_plateau_center) / beam.cdd_plateau_width));
}

void record_value(const std::string& name, double value) {
  std::ostringstream text;
  text.precision(8);
  text << value;
  ::testing::Test::RecordProperty(name, text.str());
}

class LabSim : public pychron::testing::VirtualTimeTest {
 protected:
  // The five-run queue is half a minute under the thread sanitizer; ctest
  // gives a test sixty.
  LabSim() : VirtualTimeTest(55s) {}

  void SetUp() override {
    if (!scripting::make_script_host()->available()) GTEST_SKIP() << "needs embedded Python to run the example scripts";
  }
};

// --- the tests --------------------------------------------------------------

// One air shot, seen in the Ar40 beam from before the inlet opens to after
// the pump-out.
TEST_F(LabSim, AnAirRunsSignalRisesAtTheInletAndFallsAtThePump) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const LabNumbers lab_is = numbers_of(lab.settings());
  const auto h1 = lab.beam()->detector(kFaraday);
  ASSERT_TRUE(h1) << h1.error().what;

  const auto runs = lab.run(lab.rows({1}));
  ASSERT_EQ(runs.size(), 1u);
  const auto& rec = runs[0].record;
  EXPECT_EQ(rec.identity.analysis_type, "air");
  const auto ar40 = measured(rec, "Ar40", kFaraday);
  ASSERT_TRUE(ar40);
  const auto* sniff = trace_of(rec, "sniff", "Ar40", kFaraday);
  ASSERT_NE(sniff, nullptr);

  // The plan opened the inlet once and shut it once; a record's times count
  // from the shutting (the plan's time zero).
  const auto opened = lab.valve_times(kInlet, ValveState::Open);
  const auto shut = lab.valve_times(kInlet, ValveState::Closed);
  ASSERT_EQ(opened.size(), 2u) << "the plan's inlet, then the pump-out";
  ASSERT_EQ(shut.size(), 2u);
  const double open_at = seconds(opened[0] - shut[0]);
  const double equilibration = -open_at;
  record_value("equilibration_s", equilibration);
  record_value("inlet_tau_s", lab_is.inlet_tau());
  EXPECT_GT(equilibration, 5 * lab_is.inlet_tau()) << "the plan equilibrates for several of the inlet's time constants";

  // Before the inlet: the source has been shut since the lab was built
  // pumped, and holds what a pumped volume does and what its walls and its
  // memory have given since. That is all the beam reads above its baseline.
  int before = 0;
  for (std::size_t i = 0; i < sniff->t.size(); ++i) {
    if (!(sniff->t[i] < open_at)) continue;
    ++before;
    const double shut_for = lab.since_start(shut[0]) + sniff->t[i];
    const double level = lab_is.pumped + lab_is.rise * shut_for;
    EXPECT_NEAR(sniff->v[i], ar40->baseline + level, 5 * faraday_sigma(*h1, level)) << "at " << sniff->t[i] << " s";
    EXPECT_LT(level, 1e-3 * ar40->value()) << "a pumped source is nothing beside a shot";
    record_value("before_inlet_fA_" + std::to_string(before), sniff->v[i]);
    record_value("before_inlet_expected_fA_" + std::to_string(before), ar40->baseline + level);
  }
  EXPECT_GE(before, 2) << "the sniff starts before the inlet opens";

  // At the inlet the signal rises to what the measurement then finds, as
  // 1 - exp(-t / tau): the log of what is still missing falls on a line of
  // slope -1 / tau. Readings within 2 % of the end are left out (the noise
  // of a reading is 0.1 % of it).
  const double full = ar40->signal.intercept;
  reduction::Series missing;
  for (std::size_t i = 0; i < sniff->t.size(); ++i) {
    const double left = full - sniff->v[i];
    if (sniff->t[i] > open_at && left > 0.02 * full) {
      missing.x.push_back(sniff->t[i]);
      missing.y.push_back(std::log(left));
    }
  }
  ASSERT_GE(missing.x.size(), 3u) << "the rise is seen in the sniff";
  const auto decay = line_through(missing);
  ASSERT_TRUE(decay);
  ASSERT_LT(decay->slope, 0.0);
  const double tau = -1.0 / decay->slope;
  record_value("rise_tau_s", tau);
  EXPECT_NEAR(tau, lab_is.inlet_tau(), 0.2 * lab_is.inlet_tau());
  EXPECT_GT(sniff->v.back(), 0.9 * full) << "and has all but arrived when the sniff ends";

  // Shut in the source, the gas is used up: the signal falls at the
  // consumption times itself, less what the walls and the memory give back.
  // Within 30 % of the first term alone, and within three standard errors of
  // the two together.
  const double used = -lab_is.consumption * (ar40->signal.mean - ar40->baseline);
  const double net = used + lab_is.rise;
  record_value("slope_fA_per_s", ar40->signal.slope);
  record_value("slope_error_fA_per_s", ar40->signal.slope_error);
  record_value("consumption_slope_fA_per_s", used);
  record_value("net_slope_fA_per_s", net);
  EXPECT_LT(ar40->signal.slope, 0.0);
  EXPECT_NEAR(ar40->signal.slope, used, 0.3 * std::abs(used));
  EXPECT_NEAR(ar40->signal.slope, net, 3 * ar40->signal.slope_error);
  EXPECT_LT(ar40->signal.slope_error, 0.1 * std::abs(used)) << "the measurement is long enough to see the slope";
  EXPECT_GT(std::abs(used), 10 * lab_is.rise) << "what the source uses of a shot is ten times what its walls give";

  // After the pump-out the source is shut again, prep is left pumping, and
  // the beam on the Ar40 peak reads its baseline.
  EXPECT_FALSE(lab.sim().valve_open(kInlet));
  EXPECT_TRUE(lab.sim().valve_open(kOutlet));
  const auto left = lab.sim().partial_pressures(kSource);
  ASSERT_TRUE(left) << left.error().what;
  const double sigma = faraday_sigma(*h1, 0.0);
  record_value("after_pump_out_true_fA", (*left)[kAr40] * lab_is.sensitivity);
  EXPECT_LT((*left)[kAr40] * lab_is.sensitivity, 5 * sigma);
  const auto peak = lab.beam()->peak_center(kFaraday, "Ar40");
  ASSERT_TRUE(peak) << peak.error().what;
  lab.beam()->set_magnet(*peak);
  const auto reading = lab.beam()->intensity(kFaraday);
  ASSERT_TRUE(reading) << reading.error().what;
  record_value("after_pump_out_reading_fA", reading->value);
  EXPECT_NEAR(reading->value, ar40->baseline, 5 * sigma);
  // The turbo's gauge is back where a pumped line is.
  const auto gauge = lab.sim().pressure("IG1");
  ASSERT_TRUE(gauge) << gauge.error().what;
  record_value("IG1_after_pump_out_mbar", *gauge);
  EXPECT_LT(*gauge, 2 * lab.settings().pumps.at("turbo").base);
}

// The air in the tank is air when it is measured: 40/36 of the two time-zero
// intercepts, each less its run's own baseline.
TEST_F(LabSim, AirHasTheAtmosphericRatio) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const auto cdd = lab.beam()->detector(kCounter);
  ASSERT_TRUE(cdd) << cdd.error().what;

  const auto runs = lab.run(lab.rows({1}));
  ASSERT_EQ(runs.size(), 1u);
  const auto ar40 = measured(runs[0].record, "Ar40", kFaraday);
  const auto ar36 = measured(runs[0].record, "Ar36", kCounter);
  ASSERT_TRUE(ar40);
  ASSERT_TRUE(ar36);
  ASSERT_GT(ar36->value(), 0.0);

  // Ar36 is on the ion counter, which at its voltage counts all but 1.5 % of
  // what reaches it: the counter's intercalibration, which a reduction
  // applies and which is no part of the gas.
  const double yield = counter_yield(*cdd);
  const double air = sim::air_ratios()[kAr40] / sim::air_ratios()[kAr36];
  const double ratio = ar40->value() / (ar36->value() / yield);
  const double error = ratio * std::hypot(ar40->error() / ar40->value(), ar36->error() / ar36->value());
  record_value("counter_yield", yield);
  record_value("ratio_40_36_as_counted", ar40->value() / ar36->value());
  record_value("ratio_40_36", ratio);
  record_value("ratio_40_36_error", error);
  EXPECT_NEAR(ratio, air, 0.02 * air);
  // The 2 % is three standard errors of this measurement or more: the queue
  // counts Ar36 for long enough.
  EXPECT_LT(3 * error, 0.02 * air);
}

// A blank is what the walls gave off: far below a shot, and not nothing.
TEST_F(LabSim, ABlankIsSmall) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const LabNumbers lab_is = numbers_of(lab.settings());

  const auto runs = lab.run(lab.rows({0, 1}));
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].record.identity.analysis_type, "blank_air");
  const auto blank = measured(runs[0].record, "Ar40", kFaraday);
  const auto air = measured(runs[1].record, "Ar40", kFaraday);
  ASSERT_TRUE(blank);
  ASSERT_TRUE(air);
  record_value("blank_fA", blank->value());
  record_value("blank_error_fA", blank->error());
  record_value("air_fA", air->value());
  record_value("air_expected_fA", lab_is.first_shot());
  EXPECT_GT(blank->value(), 3 * blank->error()) << "above zero";
  EXPECT_LT(blank->value(), 0.01 * air->value());
  // What sim.toml is tuned for.
  EXPECT_GT(blank->value(), 5.0);
  EXPECT_LT(blank->value(), 200.0);
  EXPECT_GT(air->value(), 1e4);
  EXPECT_LT(air->value(), 1e5);
  // And the shot is the tank's pressure in one pipette, spread over prep and
  // the source (a blank's worth of wall gas is a ten-thousandth of it).
  EXPECT_NEAR(air->value(), lab_is.first_shot(), 0.01 * lab_is.first_shot());
}

// Each shot takes its pipette of the tank: the next is smaller by
// V_tank / (V_tank + V_pipette).
TEST_F(LabSim, SuccessiveShotsDeclineWithTheTank) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const double depletion = numbers_of(lab.settings()).depletion();
  ASSERT_LT(depletion, 1.0);

  const auto runs = lab.run(lab.rows({1, 2, 3}));
  ASSERT_EQ(runs.size(), 3u);
  std::vector<Measured> shots;
  for (const auto& run : runs) {
    const auto ar40 = measured(run.record, "Ar40", kFaraday);
    ASSERT_TRUE(ar40);
    shots.push_back(*ar40);
  }
  record_value("depletion", depletion);
  for (std::size_t i = 1; i < shots.size(); ++i) {
    const double ratio = shots[i].value() / shots[i - 1].value();
    const double error = ratio * std::hypot(shots[i].error() / shots[i].value(), shots[i - 1].error() / shots[i - 1].value());
    record_value("ratio_" + std::to_string(i + 1) + "_" + std::to_string(i), ratio);
    record_value("ratio_error_" + std::to_string(i + 1) + "_" + std::to_string(i), error);
    EXPECT_NEAR(ratio, depletion, 3 * error) << "shot " << i + 1 << " over shot " << i;
    // Three standard errors are less than the decline itself: shots that
    // did not decline would not pass.
    EXPECT_LT(3 * error, 1.0 - depletion);
  }
}

// The pipette is what makes an air run an air run: with a script that leaves
// it alone, the same run is a blank.
TEST_F(LabSim, AScriptThatSkipsThePipetteMeasuresABlank) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();

  QueueSpec queue = lab.rows({1, 2});
  ASSERT_EQ(queue.runs.size(), 2u);
  ASSERT_EQ(queue.runs[0].extraction.script, "sim_air");
  queue.runs[0].extraction.script = "sim_blank";
  const auto runs = lab.run(queue);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].record.identity.analysis_type, "air");
  const auto skipped = measured(runs[0].record, "Ar40", kFaraday);
  const auto air = measured(runs[1].record, "Ar40", kFaraday);
  ASSERT_TRUE(skipped);
  ASSERT_TRUE(air);
  record_value("skipped_fA", skipped->value());
  record_value("air_fA", air->value());
  EXPECT_GT(skipped->value(), 3 * skipped->error()) << "above zero";
  EXPECT_LT(skipped->value(), 0.01 * air->value());
  EXPECT_GT(skipped->value(), 5.0);
  EXPECT_LT(skipped->value(), 200.0);
  EXPECT_GT(air->value(), 1e4);
}

// A pipette with both its valves open is no pipette: the tank empties into
// the line. The example interlocks the two against exactly this, so the lab
// here has the interlock taken out and a script that makes the mistake.
TEST_F(LabSim, ATankValveLeftOpenDrainsTheTank) {
  ScratchLab lab([](const fs::path& dir) {
    const fs::path config = dir / "extraction_line.toml";
    std::ifstream in(config);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    int removed = 0;
    for (const std::string_view interlock : {R"(interlocks = ["P2"])", R"(interlocks = ["P1"])"}) {
      if (const auto at = text.find(interlock); at != std::string::npos) {
        text.replace(at, interlock.size(), "interlocks = []");
        ++removed;
      }
    }
    EXPECT_EQ(removed, 2) << "the example's pipette valves are interlocked";
    std::ofstream(config) << text;
    std::ofstream(dir / "scripts" / "extraction" / "sim_air_both.py")
        << "def main():\n"
           "    close('C')\n"
           "    open('P2')\n"
           "    open('P1')\n"
           "    sleep(duration)\n"
           "    close('P1')\n"
           "    close('P2')\n";
  });
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const LabNumbers lab_is = numbers_of(lab.settings());

  QueueSpec queue = lab.rows({1, 2, 3});
  ASSERT_EQ(queue.runs.size(), 3u);
  queue.runs[1].extraction.script = "sim_air_both";
  // Long enough for tank, pipette and prep to become one pressure: the two
  // valves in a row between the tank's 50 cc and prep's.
  const double through_both = lab.settings().valve_conductance / 2;
  const double drain_tau = lab_is.tank * lab_is.prep / ((lab_is.tank + lab_is.prep) * through_both);
  ASSERT_GT(seconds(queue.runs[1].extraction.duration), 20 * drain_tau);

  const auto runs = lab.run(queue);  // every run a success: no error
  ASSERT_EQ(runs.size(), 3u);
  const auto first = measured(runs[0].record, "Ar40", kFaraday);
  const auto drained = measured(runs[1].record, "Ar40", kFaraday);
  const auto after = measured(runs[2].record, "Ar40", kFaraday);
  ASSERT_TRUE(first);
  ASSERT_TRUE(drained);
  ASSERT_TRUE(after);
  record_value("first_fA", first->value());
  record_value("drained_fA", drained->value());
  record_value("after_fA", after->value());

  // Half the tank is in prep, not a pipette of it.
  EXPECT_GT(drained->value(), 10 * first->value());
  // And the tank is left with its share of what tank, pipette and prep held
  // between them, so the next shot is that much smaller: far more than one
  // shot's decline.
  const double left = lab_is.depletion() * (lab_is.tank + lab_is.pipette) / (lab_is.tank + lab_is.pipette + lab_is.prep);
  record_value("after_over_first", after->value() / first->value());
  record_value("after_over_first_expected", left);
  EXPECT_LT(after->value() / first->value(), lab_is.depletion() * lab_is.depletion());
  EXPECT_NEAR(after->value() / first->value(), left, 0.01 * left);
}

// A simulated queue is its seed and its clock, and nothing else: the same
// queue in a lab built afresh reads the same numbers, every one. The five
// runs are measured for two cycles each here instead of the queue's five:
// what would make two labs differ (a draw that depends on who asked first,
// a time taken from the wall) differs from the first reading on, and ten
// full runs are most of a minute under the thread sanitizer.
TEST_F(LabSim, TheSameQueueGivesTheSameNumbers) {
  const auto run_once = [] {
    ScratchLab lab;
    EXPECT_TRUE(lab.problem().empty()) << lab.problem();
    if (!lab.problem().empty()) return std::vector<Analysis>{};
    QueueSpec queue = lab.air_queue();
    for (auto& run : queue.runs) {
      EXPECT_TRUE(run.measurement.overrides.contains("main.cycles"));
      run.measurement.overrides["main.cycles"] = std::int64_t{2};
    }
    return lab.run(queue);
  };
  const auto first = run_once();
  const auto second = run_once();
  ASSERT_EQ(first.size(), 5u);
  ASSERT_EQ(second.size(), 5u);
  std::size_t readings = 0;
  for (std::size_t i = 0; i < first.size(); ++i) {
    const auto& a = first[i].record;
    const auto& b = second[i].record;
    ASSERT_EQ(a.data.series.size(), b.data.series.size());
    for (std::size_t s = 0; s < a.data.series.size(); ++s) {
      const auto& x = a.data.series[s];
      const auto& y = b.data.series[s];
      EXPECT_EQ(x.iso, y.iso);
      EXPECT_EQ(x.det, y.det);
      EXPECT_EQ(x.kind, y.kind);
      EXPECT_EQ(x.trace.t, y.trace.t) << "run " << i << " " << x.kind << " " << x.iso << " " << x.det;
      EXPECT_EQ(x.trace.v, y.trace.v) << "run " << i << " " << x.kind << " " << x.iso << " " << x.det;
      readings += x.trace.v.size();
    }
    // And what was made of them, and when.
    EXPECT_TRUE(a.data == b.data) << "run " << i;
    EXPECT_TRUE(a.results == b.results) << "run " << i;
    EXPECT_TRUE(a.events == b.events) << "run " << i;
    EXPECT_TRUE(a.extraction == b.extraction) << "run " << i;
    EXPECT_TRUE(a.spectrometer == b.spectrometer) << "run " << i;
    EXPECT_EQ(a.identity.timestamp, b.identity.timestamp) << "run " << i;
  }
  EXPECT_GT(readings, 1000u) << "there were readings to compare";
}

// An hour of the lab costs its arithmetic, not an hour.
TEST_F(LabSim, FiveRunsTakeNoRealTime) {
  ScratchLab lab;
  ASSERT_TRUE(lab.problem().empty()) << lab.problem();
  const TimePoint lab_began = lab.clock().now();
  const auto began = std::chrono::steady_clock::now();
  const auto runs = lab.run(lab.air_queue());
  const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
  const double simulated = seconds(lab.clock().now() - lab_began);
  ASSERT_EQ(runs.size(), 5u);
  record_value("real_s", real);
  record_value("simulated_s", simulated);
  EXPECT_LT(real, 60.0);
  EXPECT_GT(simulated, 1800.0) << "the queue was half an hour of the lab's time or more";

  // Blank, air, air, air, blank.
  const std::vector<std::string> types{"blank_air", "air", "air", "air", "blank_air"};
  for (std::size_t i = 0; i < runs.size(); ++i) {
    EXPECT_EQ(runs[i].record.identity.analysis_type, types[i]);
    const auto ar40 = measured(runs[i].record, "Ar40", kFaraday);
    ASSERT_TRUE(ar40);
    record_value("ar40_fA_" + std::to_string(i + 1), ar40->value());
    if (types[i] == "air") {
      EXPECT_GT(ar40->value(), 1e4);
      EXPECT_LT(ar40->value(), 1e5);
    } else {
      EXPECT_GT(ar40->value(), 5.0);
      EXPECT_LT(ar40->value(), 200.0);
    }
  }
}

}  // namespace
}  // namespace pychron::experiment::lab
