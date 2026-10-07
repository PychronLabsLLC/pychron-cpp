// App bring-up helper against the example sim configs on a real clock.
// Integration is 0.1 s (the sim snaps to multiples of 100 ms) so each test
// runs in about a second.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/sim/gas.hpp"
#include "pychron/sim/sim_system.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kDir(PYCHRON_EXAMPLE_CONFIGS_DIR);

double mean_on(const std::vector<Reading>& readings, const DetectorId& det) {
  double sum = 0.0;
  int n = 0;
  for (const auto& r : readings) {
    auto it = r.values.find(det);
    if (it != r.values.end() && it->second) {
      sum += it->second->mean;
      ++n;
    }
  }
  return n > 0 ? sum / n : 0.0;
}

class SpectrometerBringupSim : public ::testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    sim::BeamModelRegistry::global().clear();
    scheduler_.start();
  }
  void TearDown() override {
    // stop() only joins the dispatcher: a poll already on a worker must finish
    // before the spectrometer it uses goes.
    scheduler_.stop();
    scheduler_.wait_idle();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
  }

  Result<std::unique_ptr<Spectrometer>> load(const std::filesystem::path& path, SpectrometerBringup options) {
    return load_spectrometer_for_app(path, SpectrometerContext{clock_, scheduler_, bus_}, options);
  }

  SteadyClock clock_;
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_, Scheduler::Options{2}};
  std::unique_ptr<Spectrometer> spec_;
};

TEST_P(SpectrometerBringupSim, LoadsBothSimConfigs) {
  auto spec = load(kDir / GetParam(), SpectrometerBringup{.sim_beam_from_table = true});
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  spec_ = std::move(*spec);
  EXPECT_FALSE(spec_->config().detectors.empty());
}

TEST_P(SpectrometerBringupSim, PositionedIsotopeGivesSignalOnChosenDetector) {
  auto spec = load(kDir / GetParam(), SpectrometerBringup{.sim_beam_from_table = true});
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  spec_ = std::move(*spec);
  ASSERT_TRUE(spec_->acquisition().start(100ms).has_value());

  auto pos = spec_->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(pos.has_value()) << pos.error().what;
  auto on_peak = spec_->acquire(3);
  ASSERT_TRUE(on_peak.has_value()) << on_peak.error().what;

  auto off = spec_->position(PositionTarget{NativeUnits{pos->native + 0.5}, "H1"});
  ASSERT_TRUE(off.has_value()) << off.error().what;
  auto off_peak = spec_->acquire(3);
  ASSERT_TRUE(off_peak.has_value()) << off_peak.error().what;
  spec_->acquisition().stop();

  EXPECT_GT(mean_on(*on_peak, "H1"), 10.0 * std::max(mean_on(*off_peak, "H1"), 1.0));
}

TEST_P(SpectrometerBringupSim, ExampleConfigIsSimulated) {
  auto data = cfg::load_spectrometer(kDir / GetParam());
  ASSERT_TRUE(data.has_value()) << data.error().what;
  EXPECT_TRUE(is_simulated(*data));
}

TEST_P(SpectrometerBringupSim, RequireSimAcceptsSimConfig) {
  auto spec = load(kDir / GetParam(), SpectrometerBringup{.sim_beam_from_table = true, .require_sim = true});
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  spec_ = std::move(*spec);
}

INSTANTIATE_TEST_SUITE_P(SimConfigs, SpectrometerBringupSim,
                         ::testing::Values("spectrometer.sim-integrated.toml", "spectrometer.sim-legacy.toml"));

class SpectrometerBringupMisc : public ::testing::Test {
 protected:
  void TearDown() override { sim::BeamModelRegistry::global().clear(); }
  SteadyClock clock_;
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_, Scheduler::Options{2}};
};

TEST_F(SpectrometerBringupMisc, MissingFileIsError) {
  auto spec = load_spectrometer_for_app(kDir / "no-such-spectrometer.toml",
                                        SpectrometerContext{clock_, scheduler_, bus_}, SpectrometerBringup{.sim_beam_from_table = true});
  EXPECT_FALSE(spec.has_value());
}

TEST_F(SpectrometerBringupMisc, WithoutSimFlagRegistryUntouched) {
  auto sentinel = std::make_shared<sim::BeamModel>(clock_);
  sim::BeamModelRegistry::global().set("default", sentinel);
  auto spec = load_spectrometer_for_app(kDir / "spectrometer.sim-integrated.toml",
                                        SpectrometerContext{clock_, scheduler_, bus_});
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  EXPECT_EQ(sim::BeamModelRegistry::global().acquire("default", clock_), sentinel);
}

// The example line and the example spectrometer on one clock set by hand;
// nothing is started, and the beam the drivers share is read directly.
class SpectrometerBringupLine : public ::testing::Test {
 protected:
  void SetUp() override {
    sim::BeamModelRegistry::global().clear();
    systems::ExtractionLine::Options options;
    options.clock = &clock_;
    options.force_sim = true;
    options.sim = sim_;
    // The fixture's own numbers and no others: named and empty, there is no
    // sim file, so the example's tuned sim.toml beside the line is not read.
    options.sim_file = sim_file_;
    // And the valve states of nobody's earlier run in the examples folder.
    state_file_ = std::filesystem::temp_directory_path() /
                  ("pychron-bringup-line-" + std::to_string(std::random_device{}()) + ".state.toml");
    options.state_file = state_file_;
    auto line = systems::ExtractionLine::load(kDir / "extraction_line.toml", kDir / "canvas.toml", options);
    ASSERT_TRUE(line) << line.error().what;
    line_ = std::move(*line);
    ASSERT_NE(line_->sim(), nullptr);
  }
  void TearDown() override {
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
    line_.reset();
    if (!sim_file_.empty()) std::filesystem::remove(sim_file_);
    std::filesystem::remove(state_file_);
  }

  Result<std::unique_ptr<Spectrometer>> load(sim::SimSystem* line_sim) {
    return load_spectrometer_for_app(kDir / "spectrometer.sim-integrated.toml",
                                     SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
                                     SpectrometerBringup{.sim_beam_from_table = true, .line_sim = line_sim});
  }
  // The beam the bring-up registered, on the peak of Ar40 on H1.
  std::shared_ptr<sim::BeamModel> beam_on_ar40() {
    auto beam = sim::BeamModelRegistry::global().acquire("default", clock_);
    auto center = beam->peak_center("H1", "Ar40");
    EXPECT_TRUE(center) << center.error().what;
    if (center) beam->set_magnet(*center);
    return beam;
  }
  // Mean of `n` readings of H1, a millisecond apart.
  double mean_h1(sim::BeamModel& beam, int n = 100) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
      clock_.advance(1ms);
      sum += beam.intensity("H1")->value;
    }
    return sum / n;
  }
  // A sim.toml of its own for the line to read.
  void write_sim_file(const std::string& text) {
    sim_file_ = std::filesystem::temp_directory_path() /
                ("pychron-bringup-sim-" + std::to_string(std::random_device{}()) + ".toml");
    std::ofstream(sim_file_) << text;
  }

  // The source keeps what it is given: walls, memory and consumption off.
  static sim::SimSettings still() {
    sim::SimSettings s;
    s.outgassing = 0.0;
    s.outgassing_active = 0.0;
    s.source = {1e12, 0.0, 0.0};
    return s;
  }

  ManualClock clock_;
  sim::SimSettings sim_ = still();
  std::filesystem::path sim_file_;
  std::filesystem::path state_file_;
  std::unique_ptr<systems::ExtractionLine> line_;
  std::unique_ptr<Spectrometer> spec_;
};

TEST_F(SpectrometerBringupLine, ALineSimFeedsTheBeam) {
  auto spec = load(line_->sim());
  ASSERT_TRUE(spec) << spec.error().what;
  spec_ = std::move(*spec);
  const auto source = line_->sim()->spectrometer_volume();
  ASSERT_TRUE(source);

  // Air with 1e-8 mbar of Ar40 in the source, at 1e12 fA/mbar.
  ASSERT_TRUE(line_->sim()->set_composition(*source, sim::with_ar40(sim::air_ratios(), 1e-8)));
  auto beam = beam_on_ar40();
  EXPECT_NEAR(mean_h1(*beam), 1e4, 1e4 * 0.01);
  // And its Ar36 where Ar36 is: 1e4 / 298.56.
  beam->set_magnet(*beam->peak_center("H1", "Ar36"));
  EXPECT_NEAR(mean_h1(*beam), 1e4 / 298.56, 1.0);

  // Nothing in the source: the baseline, which is nothing unless sim.toml gives one.
  ASSERT_TRUE(line_->sim()->set_composition(*source, sim::Composition{}));
  beam->set_magnet(*beam->peak_center("H1", "Ar40"));
  EXPECT_NEAR(mean_h1(*beam), 0.0, 0.5);
}

// The fixture's numbers are the ones in force, not the example sim.toml's
// (1e-10 mbar to start with, walls that give gas off): a volume behind closed
// valves starts at the default pressure and holds it exactly for an hour.
TEST_F(SpectrometerBringupLine, TheFixturesOwnNumbersAreInForce) {
  EXPECT_TRUE(line_->sim()->settings().file.empty()) << line_->sim()->settings().file;
  const auto source = line_->sim()->spectrometer_volume();
  ASSERT_TRUE(source);
  const double before = *line_->sim()->pressure(*source);
  EXPECT_DOUBLE_EQ(before, 1e-8);
  clock_.advance(1h);
  EXPECT_EQ(*line_->sim()->pressure(*source), before);
  EXPECT_DOUBLE_EQ(*line_->sim()->pressure("prep"), 1e-8);
}

TEST_F(SpectrometerBringupLine, WithoutALineSimTheBeamIsAsBefore) {
  auto spec = load(nullptr);
  ASSERT_TRUE(spec) << spec.error().what;
  spec_ = std::move(*spec);
  const auto source = line_->sim()->spectrometer_volume();
  ASSERT_TRUE(source);
  auto beam = beam_on_ar40();
  // The fixed argon, whatever the line holds.
  EXPECT_NEAR(mean_h1(*beam), 1e6, 1e6 * 0.01);
  ASSERT_TRUE(line_->sim()->set_composition(*source, sim::Composition{}));
  EXPECT_NEAR(mean_h1(*beam), 1e6, 1e6 * 0.01);
}

// A registry still holding the beam when the line goes: a late reading is
// the baseline, not a crash.
TEST_F(SpectrometerBringupLine, ABeamReadAfterItsLineIsGoneReadsNoGas) {
  auto spec = load(line_->sim());
  ASSERT_TRUE(spec) << spec.error().what;
  spec_ = std::move(*spec);
  ASSERT_TRUE(line_->sim()->set_composition(*line_->sim()->spectrometer_volume(),
                                            sim::with_ar40(sim::air_ratios(), 1e-8)));
  auto beam = beam_on_ar40();
  EXPECT_NEAR(mean_h1(*beam), 1e4, 1e4 * 0.01);
  spec_.reset();
  line_.reset();
  EXPECT_NEAR(mean_h1(*beam), 0.0, 0.5);
}

// A line with no spectrometer volume (one loaded without its canvas, say)
// leaves the beam its fixed gas, and its detectors still get their baselines.
TEST_F(SpectrometerBringupLine, BaselinesApplyEvenWithoutASpectrometerVolume) {
  sim::SimTopology topology;
  topology.volumes = {{"prep", 50.0}};
  sim::SimSettings settings = still();
  settings.detectors["H1"] = {50.0, 2.0};
  sim::SimSystem line(clock_, topology, settings);
  ASSERT_FALSE(line.spectrometer_volume());
  sim::BeamModel beam(clock_);
  beam.ensure_detector("H1");
  auto fed = feed_beam_from_line(beam, line);
  ASSERT_TRUE(fed) << fed.error().what;
  EXPECT_EQ(beam.detector("H1")->baseline, 50.0);
  EXPECT_EQ(beam.detector("H1")->baseline_drift_per_h, 2.0);
  beam.set_magnet(*beam.peak_center("H1", "Ar40"));
  EXPECT_NEAR(mean_h1(beam), 1e6 + 50.0, 1e6 * 0.01);
}

// The answer says what the beam was left reading, for a caller to tell its
// user of a spectrometer that is not joined to the line.
TEST_F(SpectrometerBringupLine, TheFeedSaysWhetherTheBeamReadsTheLine) {
  sim::BeamModel beam(clock_);
  beam.ensure_detector("H1");
  auto joined = feed_beam_from_line(beam, *line_->sim());
  ASSERT_TRUE(joined) << joined.error().what;
  EXPECT_EQ(*joined, BeamFeed::LineGas);

  sim::SimTopology topology;
  topology.volumes = {{"prep", 50.0}};
  sim::SimSystem no_source(clock_, topology, still());
  sim::BeamModel alone(clock_);
  alone.ensure_detector("H1");
  auto fixed = feed_beam_from_line(alone, no_source);
  ASSERT_TRUE(fixed) << fixed.error().what;
  EXPECT_EQ(*fixed, BeamFeed::FixedGas);

  // And through the bring-up, to whoever asks.
  BeamFeed fed = BeamFeed::FixedGas;
  auto spec = load_spectrometer_for_app(
      kDir / "spectrometer.sim-integrated.toml", SpectrometerContext{clock_, line_->scheduler(), line_->bus()},
      SpectrometerBringup{.sim_beam_from_table = true, .line_sim = line_->sim(), .fed = &fed});
  ASSERT_TRUE(spec) << spec.error().what;
  spec_ = std::move(*spec);
  EXPECT_EQ(fed, BeamFeed::LineGas);
}

// One seed for the simulated lab: a line given a seed gives it to the beam,
// and a line left at the default leaves the beam the seed it was built with.
TEST_F(SpectrometerBringupLine, TheLinesSeedSeedsTheBeam) {
  sim::SimTopology topology;
  topology.volumes = {{"source", 50.0, sim::SimRole::Spectrometer}};
  // Off every peak: the readings are noise and nothing else.
  const auto five = [](sim::BeamModel& beam, ManualClock& clock) {
    beam.set_magnet(34.2 / 5.0);
    std::vector<double> out;
    for (int i = 0; i < 5; ++i) {
      clock.advance(1ms);
      out.push_back(beam.intensity("H1")->value);
    }
    return out;
  };
  // What a beam of each seed reads, each on a clock of its own from zero.
  const auto of_seed = [&](std::uint64_t seed, const sim::SimSettings* line_settings) {
    ManualClock clock;
    sim::BeamSettings settings;
    settings.seed = seed;
    sim::BeamModel beam(clock, settings);
    beam.ensure_detector("H1");
    if (line_settings != nullptr) {
      sim::SimSystem line(clock, topology, *line_settings);
      auto fed = feed_beam_from_line(beam, line);
      EXPECT_TRUE(fed) << fed.error().what;
      // No gas of the line's off the peaks either; and the line is gone.
    }
    return five(beam, clock);
  };
  const std::uint64_t standard = sim::BeamSettings{}.seed;
  ASSERT_EQ(standard, sim::SimSettings{}.seed) << "one default seed for the gauges and the detectors";
  const auto by_default = of_seed(standard, nullptr);
  const auto of_seven = of_seed(7, nullptr);
  ASSERT_NE(by_default, of_seven);

  // A line with no seed of its own: the beam is as it was, whatever its seed.
  sim::SimSettings settings = still();
  EXPECT_EQ(of_seed(standard, &settings), by_default);
  EXPECT_EQ(of_seed(7, &settings), of_seven);
  // A line given one: the beam reads as a beam built with it.
  settings.seed = 7;
  EXPECT_EQ(of_seed(standard, &settings), of_seven);
  settings.seed = 8;
  EXPECT_NE(of_seed(standard, &settings), of_seven);
  EXPECT_EQ(of_seed(standard, &settings), of_seed(8, nullptr));
}

class SpectrometerBringupLineSeed : public SpectrometerBringupLine {
 protected:
  void SetUp() override {
    write_sim_file("[defaults]\nseed = 7\n");
    SpectrometerBringupLine::SetUp();
  }
};

// And from sim.toml, through the bring-up the applications use.
TEST_F(SpectrometerBringupLineSeed, ASeedInSimTomlReachesTheDetectors) {
  ASSERT_EQ(line_->sim()->settings().seed, 7u);
  auto spec = load(line_->sim());
  ASSERT_TRUE(spec) << spec.error().what;
  spec_ = std::move(*spec);
  ASSERT_TRUE(line_->sim()->set_composition(*line_->sim()->spectrometer_volume(), sim::Composition{}));
  auto beam = beam_on_ar40();
  // A beam built at the same instant with that seed and no line reads the
  // same noise (the source is empty, and neither has a baseline).
  sim::BeamSettings settings;
  settings.seed = 7;
  sim::BeamModel seven(clock_, settings);
  sim::BeamModel standard(clock_);
  for (auto* m : {&seven, &standard}) {
    m->ensure_detector("H1");
    m->set_magnet(34.2 / 5.0);
  }
  for (int i = 0; i < 5; ++i) {
    clock_.advance(1ms);
    const double read = beam->intensity("H1")->value;
    EXPECT_EQ(read, seven.intensity("H1")->value);
    EXPECT_NE(read, standard.intensity("H1")->value);
  }
}

// A reading's instant is the line's time: a beam on another clock is refused.
TEST_F(SpectrometerBringupLine, ABeamOnAnotherClockIsRefused) {
  ManualClock other;
  sim::BeamModel beam(other);
  beam.ensure_detector("H1");
  auto fed = feed_beam_from_line(beam, *line_->sim());
  ASSERT_FALSE(fed);
  EXPECT_EQ(fed.error().kind, ErrorKind::Config);
  EXPECT_NE(fed.error().what.find("different clocks"), std::string::npos) << fed.error().what;
  // And nothing was done to it: it still reads its own argon.
  beam.set_magnet(*beam.peak_center("H1", "Ar40"));
  EXPECT_NEAR(beam.intensity("H1")->value, 1e6, 1e6 * 0.01);
}

// Refused for one detector, the beam is as it was for all of them.
TEST_F(SpectrometerBringupLine, ARefusedFeedChangesNothing) {
  sim::SimSettings settings = still();
  settings.compositions["source"] = sim::with_ar40(sim::air_ratios(), 1e-8);
  sim::SimTopology topology;
  topology.volumes = {{"source", 50.0, sim::SimRole::Spectrometer}};
  sim::BeamModel beam(clock_);
  beam.ensure_detector("AX");
  beam.ensure_detector("H1");
  const auto unchanged = [&](const char* why) {
    EXPECT_EQ(beam.detector("AX")->baseline, 0.0) << why;
    EXPECT_EQ(beam.detector("H1")->baseline, 0.0) << why;
    beam.set_magnet(*beam.peak_center("H1", "Ar40"));
    EXPECT_NEAR(mean_h1(beam), 1e6, 1e6 * 0.01) << why << ": the fixed gas, not the line's 1e4 fA";
  };

  // AX is good and comes first by name; H1's baseline is no number.
  settings.detectors = {{"AX", {5.0, 0.0}}, {"H1", {std::numeric_limits<double>::quiet_NaN(), 0.0}}};
  {
    sim::SimSystem line(clock_, topology, settings);
    auto fed = feed_beam_from_line(beam, line);
    ASSERT_FALSE(fed);
    EXPECT_EQ(fed.error().kind, ErrorKind::Config);
    EXPECT_NE(fed.error().what.find("detectors.H1"), std::string::npos) << fed.error().what;
    unchanged("a baseline that is not finite");
  }
  // AX is good; ZZ is not a detector.
  settings.detectors = {{"AX", {5.0, 0.0}}, {"ZZ", {1.0, 0.0}}};
  {
    sim::SimSystem line(clock_, topology, settings);
    auto fed = feed_beam_from_line(beam, line);
    ASSERT_FALSE(fed);
    EXPECT_NE(fed.error().what.find("detectors.ZZ"), std::string::npos) << fed.error().what;
    unchanged("an unknown detector");
  }
}

class SpectrometerBringupLineBaselines : public SpectrometerBringupLine {
 protected:
  void SetUp() override {
    write_sim_file("[detectors.H1]\nbaseline = 50\nbaseline_drift_per_h = 2\n");
    SpectrometerBringupLine::SetUp();
  }
};

TEST_F(SpectrometerBringupLineBaselines, SimTomlBaselinesReachTheDetectors) {
  auto spec = load(line_->sim());
  ASSERT_TRUE(spec) << spec.error().what;
  spec_ = std::move(*spec);
  ASSERT_TRUE(line_->sim()->set_composition(*line_->sim()->spectrometer_volume(), sim::Composition{}));
  auto beam = beam_on_ar40();
  EXPECT_NEAR(mean_h1(*beam), 50.0, 0.5);
  clock_.advance(1h);
  EXPECT_NEAR(mean_h1(*beam), 52.0, 0.5);
  // The other detectors have none.
  EXPECT_EQ(beam->detector("AX")->baseline, 0.0);
}

class SpectrometerBringupLineUnknownDetector : public SpectrometerBringupLine {
 protected:
  void SetUp() override {
    write_sim_file("[detectors.H9]\nbaseline = 50\n");
    SpectrometerBringupLine::SetUp();
  }
};

TEST_F(SpectrometerBringupLineUnknownDetector, AnUnknownDetectorInSimTomlIsRefused) {
  auto spec = load(line_->sim());
  ASSERT_FALSE(spec);
  EXPECT_EQ(spec.error().kind, ErrorKind::Config);
  // The file, the key, and the detectors there are.
  EXPECT_NE(spec.error().what.find(sim_file_.generic_string()), std::string::npos) << spec.error().what;
  EXPECT_NE(spec.error().what.find("detectors.H9"), std::string::npos) << spec.error().what;
  EXPECT_NE(spec.error().what.find("H1"), std::string::npos) << spec.error().what;
  EXPECT_NE(spec.error().what.find("CDD"), std::string::npos) << spec.error().what;
  // With no line given, the same file's detectors are nobody's business.
  auto alone = load(nullptr);
  EXPECT_TRUE(alone) << alone.error().what;
}

cfg::SpectrometerData example_data() {
  auto data = cfg::load_spectrometer(kDir / "spectrometer.sim-legacy.toml");
  if (!data) ADD_FAILURE() << data.error().what;
  return std::move(*data);
}

TEST_F(SpectrometerBringupMisc, RealTransportKindIsNotSimulated) {
  auto data = example_data();
  data.config.transports.at("adc").kind = cfg::TransportKind::Tcp;
  EXPECT_FALSE(is_simulated(data));
}

TEST_F(SpectrometerBringupMisc, RealDriverKindIsNotSimulated) {
  auto data = example_data();
  data.config.drivers.at("faradays").kind = "adc_bank";
  EXPECT_FALSE(is_simulated(data));
}

TEST_F(SpectrometerBringupMisc, NoTransportsAndSimDriversIsSimulated) {
  auto data = example_data();
  data.config.transports.clear();
  EXPECT_TRUE(is_simulated(data));
}

TEST_F(SpectrometerBringupMisc, RequireSimRefusesRealTransportAndLeavesRegistry) {
  auto sentinel = std::make_shared<sim::BeamModel>(clock_);
  sim::BeamModelRegistry::global().set("default", sentinel);
  auto data = example_data();
  data.config.transports.at("adc").kind = cfg::TransportKind::Tcp;
  auto spec = load_spectrometer_for_app(std::move(data), SpectrometerContext{clock_, scheduler_, bus_},
                                        SpectrometerBringup{.sim_beam_from_table = true, .require_sim = true});
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().kind, ErrorKind::Config);
  EXPECT_NE(spec.error().what.find("transport 'adc'"), std::string::npos) << spec.error().what;
  EXPECT_EQ(sim::BeamModelRegistry::global().acquire("default", clock_), sentinel);
}

TEST_F(SpectrometerBringupMisc, RequireSimRefusesRealDriverKindAndLeavesRegistry) {
  auto sentinel = std::make_shared<sim::BeamModel>(clock_);
  sim::BeamModelRegistry::global().set("default", sentinel);
  auto data = example_data();
  data.config.drivers.at("faradays").kind = "adc_bank";
  auto spec = load_spectrometer_for_app(std::move(data), SpectrometerContext{clock_, scheduler_, bus_},
                                        SpectrometerBringup{.sim_beam_from_table = true, .require_sim = true});
  ASSERT_FALSE(spec.has_value());
  EXPECT_EQ(spec.error().kind, ErrorKind::Config);
  EXPECT_NE(spec.error().what.find("driver 'faradays'"), std::string::npos) << spec.error().what;
  EXPECT_NE(spec.error().what.find("adc_bank"), std::string::npos) << spec.error().what;
  EXPECT_EQ(sim::BeamModelRegistry::global().acquire("default", clock_), sentinel);
}

}  // namespace
