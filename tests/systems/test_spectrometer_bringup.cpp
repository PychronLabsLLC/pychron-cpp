// App bring-up helper against the example sim configs on a real clock.
// Integration is 0.1 s (the sim snaps to multiples of 100 ms) so each test
// runs in about a second.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

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
    if (!sim_file_.empty()) options.sim_file = sim_file_;
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
