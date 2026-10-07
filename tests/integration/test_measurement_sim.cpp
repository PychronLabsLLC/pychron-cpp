// End-to-end measurement path under simulation: the example extraction line
// (SimSystem) and the integrated sim spectrometer (BeamModel) from
// configs/examples, a plan resolved through the shipped [aliases] and detector
// config, and the MeasurementEngine driving both through the real adapters.
// Time is a ManualClock pumped by a helper thread that also drives the
// spectrometer's Scheduler.
//
// The example field table is laid out so that with Ar40 on H1, Ar39 sits on
// H2; Ar36 is measured on CDD by a separate hop.

#include <gtest/gtest.h>

#include <random>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>

#include "sim_pump.hpp"
#include "virtual_time.hpp"
#include "pychron/core/virtual_clock.hpp"
#include "pychron/systems/jobs/job_runner.hpp"
#include "pychron/core/config/loader.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/executor/executor.hpp"
#include "pychron/experiment/plan/plan_library.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/measurement/results.hpp"
#include "pychron/experiment/record/serialize.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/canvas/loader.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/config_loader.hpp"

using namespace pychron;
using namespace pychron::experiment;
using namespace pychron::experiment::measurement;
using namespace std::chrono_literals;
using collect::SeriesKind;

namespace {

const std::filesystem::path kDir(PYCHRON_EXAMPLE_CONFIGS_DIR);
const std::filesystem::path kPlans(PYCHRON_PLAN_FIXTURES_DIR);

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

MeasurementInputs inputs(plan::MeasurementPlan p, std::string run_id, ConditionalSet conditionals = {}) {
  MeasurementInputs in;
  in.plan = std::move(p);
  in.conditionals = std::move(conditionals);
  in.run_id = std::move(run_id);
  return in;
}

using pychron::testing::Pump;

// No peak-center job yet in this test: centering is covered by the spectrometer
// sim test and the peak_center unit.
constexpr const char* kPlan = R"(
[plan]
name = "sim_multicollect"
instrument_family = "sim"

[detectors]
reference = "H1"

[equilibration]
inlet = "@valves.inlet"
outlet = "@valves.outlet"
time_s = "@extraction.eqtime"
inlet_delay_s = 3

[sniff]
enabled = true
counts = 10
integration_s = 1

[baseline]
after = true
counts = 10
mass = 34.2
settle_s = 5
integration_s = 1

[main]
cycles = 2
integration_s = 1

[[main.hops]]
positions = { Ar40 = "H1", Ar39 = "H2" }
counts = 15
settle_s = 2

[[main.hops]]
positions = { Ar36 = "CDD" }
counts = 15
settle_s = 2
position = { isotope = "Ar36", detector = "CDD" }

[fits]
signal = { default = "linear" }
baseline = { default = "average" }

[parameters]
expose = ["main.cycles"]
)";

class MeasurementSim : public ::testing::Test {
 protected:
  void SetUp() override {
    // Extraction line: settle times zeroed so valve moves do not wait.
    auto cfg = config::load_system_config(kDir / "extraction_line.toml");
    ASSERT_TRUE(cfg) << cfg.error().what;
    for (auto& v : cfg->valves) v.settle_ms = 0;
    aliases_ = std::make_unique<SystemConfigAliases>(*cfg);
    auto canvas = canvas::load_canvas(kDir / "canvas.toml");
    ASSERT_TRUE(canvas) << canvas.error().what;
    systems::ExtractionLine::Options line_options;
    line_options.clock = &clock_;
    line_options.scheduler.threads = 0;
    line_options.run_scheduler = false;
    auto line = systems::ExtractionLine::create(std::move(*cfg), std::move(*canvas), line_options);
    ASSERT_TRUE(line) << line.error().what;
    line_ = std::move(*line);
    ASSERT_TRUE(line_->start());
    valve_sub_ = line_->bus().subscribe<ValveChanged>([this](const ValveChanged& e) {
      std::lock_guard lock(mutex_);
      valve_events_.push_back(e.valve + (e.state == ValveState::Open ? " open" : " closed"));
    });

    // Spectrometer on one BeamModel whose peaks follow the config's table.
    auto data = spectrometer::cfg::load_spectrometer(kDir / "spectrometer.sim-integrated.toml");
    ASSERT_TRUE(data) << data.error().what;
    catalog_ = std::make_unique<SpectrometerCatalog>(data->config);
    auto table = spectrometer::to_field_table(data->tables.at(data->config.magnet.field_table));
    sim::BeamSettings settings;
    settings.nominal_hv = *data->config.source.nominal_hv;
    settings.table_value = [table](double mass, const std::string& det) {
      auto v = table.value_for(mass, det);
      return v ? *v : mass / 8.0;
    };
    beam_ = std::make_shared<sim::BeamModel>(clock_, settings);
    sim::BeamModelRegistry::global().set("default", beam_);
    auto spec = spectrometer::SpectrometerAssembler::assemble(
        std::move(*data), spectrometer::SpectrometerContext{clock_, scheduler_, bus_});
    ASSERT_TRUE(spec) << spec.error().what;
    spec_ = std::move(*spec);
    pump_ = std::make_unique<Pump>(clock_, scheduler_);
  }

  void TearDown() override {
    pump_.reset();
    spec_.reset();
    line_.reset();
    sim::BeamModelRegistry::global().clear();
  }

  plan::PlanResolvers resolvers() const { return plan::PlanResolvers{aliases_.get(), catalog_.get()}; }

  ManualClock clock_{TimePoint{} + 1000s};
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_, Scheduler::Options{0}};
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<SystemConfigAliases> aliases_;
  std::unique_ptr<SpectrometerCatalog> catalog_;
  std::shared_ptr<sim::BeamModel> beam_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
  std::unique_ptr<Pump> pump_;
  SignalBus::Subscription valve_sub_;
  std::mutex mutex_;
  std::vector<std::string> valve_events_;
};

TEST_F(MeasurementSim, ShippedConfigsResolveTheExamplePlan) {
  auto tmpl = plan::parse_plan_template(read_file(kPlans / "multicollect.toml"), "multicollect.toml");
  ASSERT_TRUE(tmpl) << tmpl.error().what;
  auto loaded = plan::load_plan(*tmpl, {}, resolvers());
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_EQ(loaded->plan.equilibration.inlet, "B");
  EXPECT_EQ(loaded->plan.equilibration.outlet, "C");
  EXPECT_DOUBLE_EQ(loaded->plan.equilibration.time_s, 20);
  EXPECT_EQ(loaded->plan.sniff.counts, 20);

  // A detector the spectrometer does not have is caught by the catalog.
  auto text = read_file(kPlans / "multicollect.toml");
  text.replace(text.find("Ar36 = \"CDD\""), 12, "Ar36 = \"IC9\"");
  auto bad = plan::parse_plan_template(text, "bad.toml");
  ASSERT_TRUE(bad);
  auto r = plan::load_plan(*bad, {}, resolvers());
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().what.find("IC9"), std::string::npos) << r.error().what;
}

TEST_F(MeasurementSim, MeasuresThroughTheRealFacades) {
  auto tmpl = plan::parse_plan_template(kPlan, "sim.toml");
  ASSERT_TRUE(tmpl) << tmpl.error().what;
  auto loaded = plan::load_plan(*tmpl, {}, resolvers());
  ASSERT_TRUE(loaded) << loaded.error().what;
  EXPECT_EQ(loaded->plan.equilibration.inlet, "B");

  SpectrometerPort port(*spec_);
  ExtractionLineValves valves(*line_);
  int overlap = 0;
  auto sub = bus_.subscribe<OverlapReady>([&](const OverlapReady&) { ++overlap; });
  EngineContext ctx{port, clock_, &valves, nullptr, nullptr, &bus_, nullptr};
  MeasurementEngine engine(ctx, inputs(loaded->plan, "sim-1"));
  scripting::CancelToken token;
  auto r = engine.run(token);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");

  // Valves: inlet opened then closed; the line ends with it closed.
  {
    std::lock_guard lock(mutex_);
    auto open = std::find(valve_events_.begin(), valve_events_.end(), "B open");
    ASSERT_NE(open, valve_events_.end());
    EXPECT_NE(std::find(open, valve_events_.end(), "B closed"), valve_events_.end());
  }
  EXPECT_EQ(line_->snapshot().valves.at("B"), ValveState::Closed);
  EXPECT_EQ(overlap, 1);
  EXPECT_NEAR(*r.data.timing.inlet_close - *r.data.timing.inlet_open, 20.0, 1.5);

  // Series: two cycles of each hop.
  EXPECT_EQ(r.data.series.at({"Ar40", "H1", SeriesKind::Signal}).v.size(), 30u);
  EXPECT_EQ(r.data.series.at({"Ar39", "H2", SeriesKind::Signal}).v.size(), 30u);
  EXPECT_EQ(r.data.series.at({"Ar36", "CDD", SeriesKind::Signal}).v.size(), 30u);
  EXPECT_EQ(r.data.series.at({"Ar40", "H1", SeriesKind::Sniff}).v.size(), 10u);
  EXPECT_EQ(r.data.counts.at("baseline.after"), 10);

  // Intercepts recover the BeamModel's gas (default argon abundances).
  auto fits = fit_results(r.data, loaded->plan.fits);
  ASSERT_TRUE(fits.errors.empty()) << fits.errors.front();
  const double ar40 = fits.results.intercepts.at("Ar40").intercept.value;
  const double ar39 = fits.results.intercepts.at("Ar39").intercept.value;
  const double ar36 = fits.results.intercepts.at("Ar36").intercept.value;
  EXPECT_NEAR(ar40, 1e6, 1e6 * 0.01);
  EXPECT_NEAR(ar39, 1e4, 1e4 * 0.01);
  EXPECT_NEAR(ar40 / ar39, 100.0, 1.5);
  EXPECT_NEAR(ar36, 3e3, 3e3 * 0.05);
  // Baseline at mass 34.2 sees no gas.
  EXPECT_LT(std::abs(fits.results.baselines.at("H1").value), 5.0);

  // Nothing left protected; CDD survived the moves across Ar40.
  for (const auto& d : spec_->detectors().states()) EXPECT_FALSE(d.protected_) << d.detector;
  EXPECT_FALSE(beam_->overloaded("CDD"));
  EXPECT_FALSE(spec_->acquisition().running());
}

TEST_F(MeasurementSim, CancelMidMeasurementLeavesTheLineSafe) {
  auto text = std::string(kPlan);
  text.replace(text.find("inlet_delay_s = 3"), 17, "inlet_delay_s = 3\nclose_inlet = false");
  auto tmpl = plan::parse_plan_template(text, "sim.toml");
  ASSERT_TRUE(tmpl) << tmpl.error().what;
  auto loaded = plan::load_plan(*tmpl, {}, resolvers());
  ASSERT_TRUE(loaded) << loaded.error().what;

  SpectrometerPort port(*spec_);
  ExtractionLineValves valves(*line_);
  EngineContext ctx{port, clock_, &valves, nullptr, nullptr, &bus_, nullptr};
  MeasurementEngine engine(ctx, inputs(loaded->plan, "sim-2"));
  scripting::CancelToken token;
  int seen = 0;
  auto sub = bus_.subscribe<collect::SeriesUpdated>([&](const collect::SeriesUpdated& e) {
    if (e.label == "main" && ++seen == 5) token.cancel();
  });
  auto r = engine.run(token);
  EXPECT_EQ(r.outcome, MeasurementOutcome::Cancelled);
  EXPECT_EQ(line_->snapshot().valves.at("B"), ValveState::Closed);
  EXPECT_FALSE(spec_->acquisition().running());
  for (const auto& d : spec_->detectors().states()) EXPECT_FALSE(d.protected_) << d.detector;
}

TEST_F(MeasurementSim, InstrumentMetricsAnswerConditionals) {
  ASSERT_TRUE(spec_->set_deflection("H1", 120).has_value());
  ASSERT_TRUE(spec_->set_active("L1", false).has_value());
  InstrumentMetrics metrics(spec_.get(), line_.get(), [](std::string_view name) -> Result<double> {
    if (name == "chiller") return 12.5;
    return fail(ErrorKind::Config, "no device");
  });
  auto check = [&](const std::string& text) {
    auto e = parse_expression(text);
    EXPECT_TRUE(e) << text;
    auto r = evaluate_check(**e, metrics, {});
    EXPECT_TRUE(r) << text << ": " << (r ? "" : r.error().what);
    return r && r->tripped;
  };
  EXPECT_TRUE(check("H1.deflection == 120"));
  EXPECT_TRUE(check("L1.inactive and not H1.inactive"));
  EXPECT_TRUE(check("gauge.IG1.pressure > 0"));
  EXPECT_TRUE(check("device.chiller < 15"));
  EXPECT_FALSE(metrics.scalar(MetricRef{MetricRef::Kind::Device, "pump", "", ""}));
  EXPECT_FALSE(metrics.scalar(MetricRef{MetricRef::Kind::Gauge, "nope", "", "pressure"}));
  EXPECT_FALSE(metrics.scalar(MetricRef{MetricRef::Kind::Isotope, "Ar40", "", ""}));
}

TEST_F(MeasurementSim, LabConditionalsActOnLiveData) {
  // The example lab's conditionals directory plus a plan truncation that the
  // sim beam (Ar40 ~ 1e6 fA on H1) trips.
  DirectoryConditionalSource source(kDir / "conditionals");
  ConditionalLibrary library(source);
  auto tmpl = plan::parse_plan_template(kPlan, "sim.toml");
  ASSERT_TRUE(tmpl);
  auto loaded = plan::load_plan(*tmpl, {}, resolvers());
  ASSERT_TRUE(loaded) << loaded.error().what;
  loaded->plan.conditionals.include = {"@conditionals.default_unknown"};
  loaded->plan.conditionals.truncations = {{"Ar40.cur > 5e5", 3}};
  RunSpec run;
  run.id.identifier = "12345";
  run.measurement.plan = "sim_multicollect";
  auto set = library.for_run(QueueSpec{}, run, loaded->plan);
  ASSERT_TRUE(set) << set.error().what;

  SpectrometerPort port(*spec_);
  ExtractionLineValves valves(*line_);
  InstrumentMetrics instrument(spec_.get(), line_.get());
  EngineContext ctx{port, clock_, &valves, nullptr, nullptr, &bus_, &instrument};
  MeasurementInputs in = inputs(loaded->plan, "sim-3", *set);
  in.analysis_type = "unknown";
  MeasurementEngine engine(ctx, in);
  scripting::CancelToken token;
  auto r = engine.run(token);
  ASSERT_EQ(r.outcome, MeasurementOutcome::Truncated) << (r.error ? r.error->what : "");
  // Installed: system (minus pre_run, which is not in-run but still listed), plan include, plan inline.
  std::set<std::string> names;
  for (const auto& c : r.installed) names.insert(c.name);
  for (const char* n : {"vacuum_excursion", "huge_signal", "no_gas", "plan.truncation[0]"})
    EXPECT_TRUE(names.contains(n)) << n;
  ASSERT_EQ(r.data.trips.size(), 1u);
  EXPECT_EQ(r.data.trips[0].name, "plan.truncation[0]");
  EXPECT_EQ(r.data.trips[0].reading, 4);
  EXPECT_EQ(r.data.series.at({"Ar40", "H1", SeriesKind::Signal}).v.size(), 4u);
  // The vacuum check read the live gauge through InstrumentMetrics without errors.
  for (const auto& e : r.conditional_errors) EXPECT_NE(e.name, "vacuum_excursion") << e.message;
  const auto rec = to_record_conditionals(r.installed, r.data.trips, r.conditional_errors);
  EXPECT_EQ(rec.tripped.size(), 1u);
  EXPECT_GT(rec.tripped[0].context.at("Ar40.cur"), 5e5);
}

TEST_F(MeasurementSim, ExecutorRunsAQueueOnTheSimLab) {
  // Plans, conditionals and records as a lab would have them.
  plan::PlanLibrary plans(resolvers());
  auto tmpl = plan::parse_plan_template(kPlan, "sim_multicollect.toml");
  ASSERT_TRUE(tmpl) << tmpl.error().what;
  plans.add(*tmpl);
  DirectoryConditionalSource source(kDir / "conditionals");
  ConditionalLibrary library(source);
  const auto scratch = std::filesystem::temp_directory_path() /
                       ("executor_sim_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                                     std::to_string(std::random_device{}()));
  persist::FilePersister files(scratch / "records");
  persist::Spool spool(scratch / "spool");
  persist::SavePipeline save(spool, files);
  persist::AliquotAllocator aliquots(files);

  // Real embedded Python when built with it; otherwise the runs have no scripts.
  auto host = scripting::make_script_host();
  scripting::MapScriptResolver scripts;
  scripts.add("extraction/sim_extract", "def main():\n    info('extracting ' + run_identifier)\n    sleep(2)\n");
  scripts.add("post_measurement/sim_pump", "def main():\n    signal_pump_time_start()\n    info('pumping')\n");

  SpectrometerPort port(*spec_);
  ExtractionLineValves valves(*line_);
  InstrumentMetrics instrument(spec_.get(), line_.get());

  executor::ExecutorContext ctx;
  auto& s = ctx.services;
  s.clock = &clock_;
  s.bus = &bus_;
  s.scripts = host.get();
  s.resolver = &scripts;
  s.spectrometer = &port;
  s.valves = &valves;
  s.instrument_metrics = &instrument;
  s.spectrometer_info = [this] {
    const auto st = spec_->snapshot();
    return run::SpectrometerInfo{st.hash_hex(), st.field_table, 1.0};
  };
  s.plans = &plans;
  s.conditionals = &library;
  s.aliquots = &aliquots;
  s.persister = &files;
  s.save = &save;
  s.instrument.mass_spectrometer = "sim";
  ctx.pre_run_metrics = &instrument;

  auto make = [&](const std::string& id, AnalysisType type) {
    RunSpec r;
    r.id.identifier = id;
    r.id.type = type;
    r.measurement.plan = "sim_multicollect";
    if (host->available()) {
      r.extraction.script = "extraction/sim_extract";
      r.post_measurement = "post_measurement/sim_pump";
    }
    return r;
  };
  QueueSpec q;
  q.name = "sim-queue";
  q.mass_spectrometer = "sim";
  q.delays = {};
  q.delays.before_analyses = q.delays.between_analyses = q.delays.after_blank = experiment::Duration{1};
  q.runs = {make("bu", AnalysisType::BlankUnknown), make("66001", AnalysisType::Unknown),
            make("66001", AnalysisType::Unknown)};
  ExperimentQueue queue(q);

  executor::ExecutorOptions opts;
  opts.state_file = scratch / "executor_state.json";
  executor::Executor ex(ctx, opts);
  auto r = ex.execute(queue);
  ASSERT_EQ(r.end, executor::QueueEnd::Completed) << r.reason;
  ASSERT_EQ(r.runs.size(), 3u);
  for (const auto& run : r.runs) EXPECT_EQ(run.state, run::RunState::Success) << run.identifier << " " << run.error.value_or("");
  EXPECT_EQ(r.runs[1].aliquot, 1);
  EXPECT_EQ(r.runs[2].aliquot, 2);

  // The records on disk carry the sim beam's argon and the run's provenance.
  for (int aliquot : {1, 2}) {
    const auto path = scratch / "records" / "66001" / ("66001-" + std::to_string(aliquot) + ".json");
    ASSERT_TRUE(std::filesystem::exists(path)) << path;
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto rec = record::from_json(text);
    ASSERT_TRUE(rec) << rec.error().what;
    EXPECT_NEAR(rec->results.intercepts.at("Ar40").intercept.value, 1e6, 1e4);
    EXPECT_FALSE(rec->spectrometer.state_hash.empty());
    EXPECT_FALSE(rec->conditionals.installed.empty());
    if (host->available()) {
      EXPECT_FALSE(rec->measurement.scripts.at("extraction").sha.empty());
    }
  }
  EXPECT_EQ(save.pending(), 0u);
  EXPECT_EQ(*executor::Executor::resume_row(scratch / "executor_state.json"), 3u);
  EXPECT_EQ(line_->snapshot().valves.at("B"), ValveState::Closed);
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
}

TEST_F(MeasurementSim, PeakCenterFindsAnOffsetPeakAndTheNextRunMeasuresOnIt) {
  // H1's true peak sits 0.025 above the field table: off the flat top.
  sim::BeamDetector h1;
  h1.name = "H1";
  h1.offset = 0.025;
  beam_->add_detector(h1);

  auto text = std::string(kPlan);
  text.replace(text.find("[baseline]"), 10,
               "[peak_center]\nbefore = true\nisotope = \"Ar40\"\ndetector = \"H1\"\nconfig = \"wide\"\n\n[baseline]");
  auto tmpl = plan::parse_plan_template(text, "sim.toml");
  ASSERT_TRUE(tmpl) << tmpl.error().what;
  auto loaded = plan::load_plan(*tmpl, {}, resolvers());
  ASSERT_TRUE(loaded) << loaded.error().what;

  jobs::PeakCenterConfig wide;
  wide.name = "wide";
  wide.window = 0.08;
  wide.step = 0.002;
  wide.integration = std::chrono::seconds(1);
  SpectrometerPort port(*spec_);
  ExtractionLineValves valves(*line_);
  SpectrometerPeakCenter peak_center(*spec_, {{"wide", wide}});
  EngineContext ctx{port, clock_, &valves, &peak_center, nullptr, &bus_, nullptr};

  const double table_before =
      *spec_->native_for(*spec_->mass_of(spectrometer::PositionTarget{spectrometer::Isotope{"Ar40"}, "H1"}), "H1");
  for (int run = 0; run < 2; ++run) {
    SCOPED_TRACE(run);
    MeasurementEngine engine(ctx, inputs(loaded->plan, "pc-" + std::to_string(run)));
    scripting::CancelToken token;
    auto r = engine.run(token);
    ASSERT_EQ(r.outcome, MeasurementOutcome::Completed) << (r.error ? r.error->what : "");
    ASSERT_EQ(r.peak_centers.size(), 1u);
    const auto& pc = r.peak_centers[0];
    ASSERT_TRUE(pc.ok) << pc.message;
    EXPECT_TRUE(pc.table_updated);
    EXPECT_NEAR(*pc.center, table_before + 0.025, 0.004);
    // Measured on the peak: the full Ar40 signal.
    auto fits = fit_results(r.data, loaded->plan.fits);
    EXPECT_NEAR(fits.results.intercepts.at("Ar40").intercept.value, 1e6, 1e4);
  }
  ASSERT_TRUE(peak_center.last());
  EXPECT_FALSE(peak_center.last()->tries.empty());
}

// ---- SpectrometerPeakCenter and the run's token ------------------------------

// A peak center long enough to be cancelled part way: 80 steps of 1 s.
jobs::PeakCenterConfig slow_peak_center() {
  jobs::PeakCenterConfig cfg;
  cfg.name = "slow";
  cfg.window = 0.08;
  cfg.step = 0.001;
  cfg.integration = std::chrono::seconds(1);
  return cfg;
}

PeakCenterRequest slow_request() {
  PeakCenterRequest request;
  request.isotope = "Ar40";
  request.detector = "H1";
  request.config = "slow";
  return request;
}

class SpectrometerPeakCenterSim : public MeasurementSim {
 protected:
  // Cancels `token` from another thread once the job is acquiring.
  Result<PeakCenterReport> cancel_part_way(SpectrometerPeakCenter& peak_center, scripting::CancelToken& token) {
    std::atomic<bool> gave_up{false};
    std::thread canceller([&] {
      gave_up = !pychron::testing::eventually_real([&] { return spec_->acquisition().running(); });
      token.cancel();
    });
    auto r = peak_center.peak_center(slow_request(), token);
    canceller.join();
    EXPECT_FALSE(gave_up) << "the job never started to acquire";
    return r;
  }
};

TEST_F(SpectrometerPeakCenterSim, CancellingTheRunCancelsTheJob) {
  SpectrometerPeakCenter peak_center(*spec_, {{"slow", slow_peak_center()}});
  scripting::CancelToken token;
  auto r = cancel_part_way(peak_center, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_FALSE(peak_center.last());
  // Nothing of the call is left on the token: a later request calls nothing.
  token.abort();
}

TEST_F(SpectrometerPeakCenterSim, CancellingTheRunCancelsTheJobOnARunner) {
  jobs::JobRunner runner(*spec_, scheduler_, bus_, clock_);
  std::mutex mutex;
  std::vector<jobs::JobState> finished;
  auto sub = bus_.subscribe<jobs::JobFinished>([&](const jobs::JobFinished& e) {
    std::lock_guard lock(mutex);
    finished.push_back(e.job.state);
  });
  SpectrometerPeakCenter peak_center(*spec_, {{"slow", slow_peak_center()}}, &runner);
  scripting::CancelToken token;
  auto r = cancel_part_way(peak_center, token);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
  EXPECT_FALSE(runner.busy());
  {
    std::lock_guard lock(mutex);
    ASSERT_EQ(finished.size(), 1u);
    EXPECT_EQ(finished[0], jobs::JobState::Cancelled);
  }
  token.abort();
}

// Cancelled before the job is registered with the runner, when there is no
// current job for the cancel to find.
TEST_F(SpectrometerPeakCenterSim, ARunAlreadyCancelledDoesNotRunTheJob) {
  for (bool with_runner : {false, true}) {
    SCOPED_TRACE(with_runner);
    jobs::JobRunner runner(*spec_, scheduler_, bus_, clock_);
    SpectrometerPeakCenter peak_center(*spec_, {{"slow", slow_peak_center()}}, with_runner ? &runner : nullptr);
    scripting::CancelToken token;
    token.cancel();
    const auto before = clock_.now();
    auto r = peak_center.peak_center(slow_request(), token);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().kind, ErrorKind::Cancelled);
    // Not one step of the 80 was measured.
    EXPECT_LT(clock_.now() - before, 60s);
    EXPECT_FALSE(runner.busy());
  }
}

// The same spectrometer on a VirtualClock: the scheduler's dispatcher runs the
// acquisition polls and the test's thread, a participant, runs the job.
class SpectrometerPeakCenterVirtual : public pychron::testing::VirtualTimeTest {
 protected:
  static VirtualClock::Options options(SpectrometerPeakCenterVirtual* self) {
    VirtualClock::Options o;
    o.stall_report_after = 200ms;
    o.on_stall = [self](std::string what) {
      std::lock_guard lock(self->mutex_);
      self->stalls_.push_back(std::move(what));
    };
    return o;
  }

  void SetUp() override {
    auto data = spectrometer::cfg::load_spectrometer(kDir / "spectrometer.sim-integrated.toml");
    ASSERT_TRUE(data) << data.error().what;
    auto table = spectrometer::to_field_table(data->tables.at(data->config.magnet.field_table));
    sim::BeamSettings settings;
    settings.nominal_hv = *data->config.source.nominal_hv;
    settings.table_value = [table](double mass, const std::string& det) {
      auto v = table.value_for(mass, det);
      return v ? *v : mass / 8.0;
    };
    beam_ = std::make_shared<sim::BeamModel>(clock_, settings);
    sim::BeamModelRegistry::global().set("default", beam_);
    auto spec = spectrometer::SpectrometerAssembler::assemble(
        std::move(*data), spectrometer::SpectrometerContext{clock_, scheduler_, bus_});
    ASSERT_TRUE(spec) << spec.error().what;
    spec_ = std::move(*spec);
    scheduler_.start();
  }

  void TearDown() override {
    scheduler_.stop();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
  }

  std::vector<std::string> stalls() {
    std::lock_guard lock(mutex_);
    return stalls_;
  }

  std::mutex mutex_;
  std::vector<std::string> stalls_;
  VirtualClock clock_{options(this)};  // before everything that is given a reference to it
  Clock::Participant test_{clock_, "test"};
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_, Scheduler::Options{0}};
  std::shared_ptr<sim::BeamModel> beam_;
  std::unique_ptr<spectrometer::Spectrometer> spec_;
};

TEST_F(SpectrometerPeakCenterVirtual, RunsWithoutAHelperThread) {
  jobs::PeakCenterConfig wide;
  wide.name = "wide";
  wide.window = 0.08;
  wide.step = 0.002;
  wide.integration = std::chrono::seconds(1);
  SpectrometerPeakCenter peak_center(*spec_, {{"wide", wide}});
  PeakCenterRequest request;
  request.isotope = "Ar40";
  request.detector = "H1";
  request.config = "wide";

  scripting::CancelToken token;
  const auto started = clock_.now();
  const auto real_started = std::chrono::steady_clock::now();
  auto r = peak_center.peak_center(request, token);
  ASSERT_TRUE(r) << r.error().what;
  EXPECT_TRUE(r->ok) << r->message;
  // 40 steps of 1 s and more of simulated time, in none of real time.
  EXPECT_GE(clock_.now() - started, 40s);
  EXPECT_LT(std::chrono::steady_clock::now() - real_started, 5s);
  // Nothing beside the job kept time standing.
  EXPECT_TRUE(stalls().empty()) << stalls().front();
}

}  // namespace
