// App bring-up helper against the example sim configs on a real clock.
// Integration is 0.1 s (the sim snaps to multiples of 100 ms) so each test
// runs in about a second.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "pychron/core/clock.hpp"
#include "pychron/core/scheduler.hpp"
#include "pychron/core/signal_bus.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
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
    spec_.reset();
    scheduler_.stop();
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
  auto spec = load(kDir / GetParam(), SpectrometerBringup{true});
  ASSERT_TRUE(spec.has_value()) << spec.error().what;
  spec_ = std::move(*spec);
  EXPECT_FALSE(spec_->config().detectors.empty());
}

TEST_P(SpectrometerBringupSim, PositionedIsotopeGivesSignalOnChosenDetector) {
  auto spec = load(kDir / GetParam(), SpectrometerBringup{true});
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
                                        SpectrometerContext{clock_, scheduler_, bus_}, SpectrometerBringup{true});
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

}  // namespace
