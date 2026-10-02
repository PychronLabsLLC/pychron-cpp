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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>

#include "pychron/core/config/loader.hpp"
#include "pychron/experiment/conditionals/library.hpp"
#include "pychron/experiment/measurement/adapters.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/measurement/results.hpp"
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

class Pump {
 public:
  Pump(ManualClock& clock, Scheduler& scheduler) : clock_(clock), scheduler_(scheduler) {
    thread_ = std::thread([this] {
      while (!done_) {
        clock_.advance(20ms);
        scheduler_.run_pending();
        std::this_thread::sleep_for(200us);
      }
    });
  }
  ~Pump() {
    done_ = true;
    thread_.join();
  }

 private:
  ManualClock& clock_;
  Scheduler& scheduler_;
  std::atomic<bool> done_{false};
  std::thread thread_;
};

// No peak-center job yet in this test: centring is covered by the spectrometer
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
  MeasurementEngine engine(ctx, MeasurementInputs{loaded->plan, {}, {}, "sim-1"});
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
  MeasurementEngine engine(ctx, MeasurementInputs{loaded->plan, {}, {}, "sim-2"});
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
  MeasurementInputs in{loaded->plan, *set, {}, "sim-3"};
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

}  // namespace
