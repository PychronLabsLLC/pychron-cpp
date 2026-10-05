// End-to-end: example spectrometer config -> SpectrometerAssembler -> sim
// drivers on one BeamModel -> position, acquire, a coarse centering scan,
// table update, re-position. Runs identically against the integrated-vendor
// and legacy-split configs. Time is a ManualClock pumped by a helper thread
// that also drives the Scheduler, so nothing waits on the wall clock.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "sim_pump.hpp"
#include "pychron/sim/spectrometer/beam_model.hpp"
#include "pychron/systems/spectrometer/assembler.hpp"
#include "pychron/systems/spectrometer/bringup.hpp"

using namespace pychron;
using namespace pychron::spectrometer;
using namespace std::chrono_literals;

namespace {

const std::filesystem::path kDir(PYCHRON_EXAMPLE_CONFIGS_DIR);
constexpr double kAr40 = 39.9623831237;

using pychron::testing::Pump;

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

class SpectrometerSim : public ::testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    auto data = cfg::load_spectrometer(kDir / GetParam());
    ASSERT_TRUE(data.has_value()) << data.error().what;
    table_ = to_field_table(data->tables.at(data->config.magnet.field_table));

    // The beam's true peak positions follow the config's field table, except
    // that H1 sits kOffset higher: the table is slightly wrong, as in life.
    sim::BeamSettings settings = beam_settings_from_config(*data);
    beam_ = std::make_shared<sim::BeamModel>(clock_, settings);
    sim::BeamModelRegistry::global().set("default", beam_);
    sim::BeamDetector h1;
    h1.name = "H1";
    h1.offset = kOffset;
    beam_->add_detector(h1);

    auto spec = SpectrometerAssembler::assemble(std::move(*data), SpectrometerContext{clock_, scheduler_, bus_});
    ASSERT_TRUE(spec.has_value()) << spec.error().what;
    spec_ = std::move(*spec);
    moved_ = bus_.subscribe<MagnetMoved>([this](const MagnetMoved& m) { moves_.push_back(m); });
    pump_ = std::make_unique<Pump>(clock_, scheduler_);
  }

  void TearDown() override {
    pump_.reset();
    spec_.reset();
    sim::BeamModelRegistry::global().clear();
  }

  static constexpr double kOffset = 0.012;
  ManualClock clock_{TimePoint{} + 1000s};
  SignalBus bus_;
  Scheduler scheduler_{clock_, &bus_, Scheduler::Options{0}};
  FieldTable table_;
  std::shared_ptr<sim::BeamModel> beam_;
  std::unique_ptr<Spectrometer> spec_;
  SignalBus::Subscription moved_;
  std::vector<MagnetMoved> moves_;
  std::unique_ptr<Pump> pump_;
};

TEST_P(SpectrometerSim, PositionAcquireCenterUpdateReposition) {
  // Position Ar40 on H1 through the full pipeline.
  auto pos = spec_->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(pos.has_value()) << pos.error().what;
  EXPECT_NEAR(pos->native, *table_.value_for(kAr40, "H1"), 1e-9);
  EXPECT_NEAR(*spec_->magnet_native(), pos->native, 1e-9);
  ASSERT_FALSE(moves_.empty());
  EXPECT_NEAR(*moves_.back().mass_on_reference, kAr40, 1e-6);
  for (const auto& d : spec_->detectors().states()) EXPECT_FALSE(d.protected_) << d.detector;

  // Acquire 10 readings; every configured detector has an entry.
  auto readings = spec_->acquire(10);
  ASSERT_TRUE(readings.has_value()) << readings.error().what;
  ASSERT_EQ(readings->size(), 10U);
  for (const auto& r : *readings) EXPECT_EQ(r.values.size(), spec_->config().detectors.size());
  const double on_table = mean_on(*readings, "H1");
  EXPECT_GT(on_table, 0.0);

  // Coarse centering scan across the peak (a stand-in for the peak-center job).
  const double start = pos->native;
  std::vector<std::pair<double, double>> scan;
  double top = 0.0;
  for (int i = -12; i <= 12; ++i) {
    const double x = start + 0.005 * i;
    ASSERT_TRUE(spec_->move_native(x).has_value());
    auto r = spec_->acquire(1);
    ASSERT_TRUE(r.has_value()) << r.error().what;
    scan.emplace_back(x, mean_on(*r, "H1"));
    top = std::max(top, scan.back().second);
  }
  double sum = 0.0;
  int n = 0;
  for (auto [x, y] : scan) {
    if (y > 0.5 * top) {
      sum += x;
      ++n;
    }
  }
  ASSERT_GT(n, 0);
  const double center = sum / n;
  EXPECT_NEAR(center, start + kOffset, 0.004);

  // Table updated from the uncorrected center; re-position lands on it.
  auto table_value = spec_->uncorrect(center, "H1");
  ASSERT_TRUE(table_value.has_value());
  ASSERT_TRUE(spec_->update_table("H1", "Ar40", *table_value).has_value());
  auto again = spec_->position(PositionTarget{Isotope{"Ar40"}, "H1"});
  ASSERT_TRUE(again.has_value()) << again.error().what;
  EXPECT_NEAR(again->native, center, 1e-9);
  auto peak = spec_->acquire(3);
  ASSERT_TRUE(peak.has_value());
  EXPECT_GT(mean_on(*peak, "H1"), 0.9 * top);

  // Snapshot is stable while nothing changes.
  auto a = spec_->snapshot();
  auto b = spec_->snapshot();
  EXPECT_EQ(a.hash, b.hash);
  EXPECT_NEAR(*a.magnet, center, 1e-9);
  for (const auto& d : a.detectors) EXPECT_FALSE(d.protected_) << d.detector;
}

INSTANTIATE_TEST_SUITE_P(BothConfigs, SpectrometerSim,
                         ::testing::Values("spectrometer.sim-integrated.toml", "spectrometer.sim-legacy.toml"),
                         [](const auto& test_info) {
                           return std::string(test_info.param).find("legacy") != std::string::npos ? "Legacy"
                                                                                               : "Integrated";
                         });

}  // namespace
