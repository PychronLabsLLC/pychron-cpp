#pragma once

// SimExtractionDevice: a laser or furnace for sim runs (spec section 7) that
// turns output power into sample temperature and temperature into gas.
//
// Physics, deliberately simple:
//   - Heating: while enabled with a non-zero output (and, for a laser, while
//     firing) the sample temperature approaches a target with a first-order
//     lag:  T(t + dt) = target + (T(t) - target) * exp(-dt / tau)
//     target = ambient + c_per_watt * watts for Percent/Watts outputs, or the
//     setpoint itself for Celsius (furnace PID). Otherwise target = ambient.
//   - Gas: every position holds gas_per_position. The fraction released
//     depends only on the peak temperature the sample has reached, via a
//     logistic centred on release_c50:
//       f(T) = L(T) - L(ambient) normalised to [0, 1],
//       L(T) = 1 / (1 + exp(-(T - release_c50) / release_width_c))
//     so holding a temperature releases nothing more; stepping hotter does.
//   - Released gas is reported to the on_gas_release hook (e.g. the sim
//     extraction line adds it to a volume, the BeamModel scales its beams).
// The target is constant between commands, so advancing lazily to
// clock.now() on every call is exact. update() advances without a command
// for callers that only poll.
//
// The sample is the stage position (laser) or the dropped sample (furnace);
// with no sample nothing is released. Stage moves and patterns take fixed
// durations on the injected Clock. Only the features in `features` are
// exposed; Celsius extraction requires Furnace.
//
// Thread-safe. The hook is called without the device lock held, on the
// thread whose call advanced the model.

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/devices/extraction/interfaces.hpp"

namespace pychron::sim {

struct SimExtractionSettings {
  std::string name = "sim_extraction";
  extraction::CapabilitySet features{extraction::Capability::Laser, extraction::Capability::Stage,
                                     extraction::Capability::Pattern};
  double max_power_watts = 75.0;  // Percent output scale
  double ambient_c = 20.0;
  double c_per_watt = 20.0;  // steady-state rise per watt
  Duration thermal_tau = std::chrono::seconds(5);
  double release_c50 = 800.0;
  double release_width_c = 100.0;
  double gas_per_position = 1.0;  // arbitrary amount units
  std::vector<std::string> positions{"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
  std::vector<std::string> trays{"default"};
  std::vector<std::string> patterns{"spiral"};
  Duration move_time = std::chrono::seconds(1);
  Duration pattern_time = std::chrono::seconds(10);
};

struct GasRelease {
  std::string position;
  double amount = 0;  // released since the previous report
  double temperature_c = 0;
  TimePoint at{};
};

using GasReleaseHook = std::function<void(const GasRelease&)>;

class SimExtractionDevice final : public extraction::IExtractionDevice,
                                  public extraction::ILaserDevice,
                                  public extraction::IFurnaceDevice,
                                  public extraction::IStage,
                                  public extraction::IPatternRunner {
 public:
  // `clock` must outlive the device.
  SimExtractionDevice(SimExtractionSettings settings, const Clock& clock);

  // Replaces any previous hook. Set before use.
  void on_gas_release(GasReleaseHook hook);
  // Advance the model to clock.now(), reporting any gas released.
  void update();
  double temperature();  // Celsius, advanced to now
  double released(std::string_view position) const;

  // IExtractionDevice
  const std::string& device_name() const override { return settings_.name; }
  Result<void> enable() override;
  Result<void> disable() override;
  Result<bool> is_enabled() override;
  Result<void> extract(double value, extraction::ExtractUnits units) override;
  Result<void> end_extract() override;
  Result<double> output() override;
  bool supports(extraction::ExtractUnits units) const override;
  extraction::ILaserDevice* laser() override;
  extraction::IFurnaceDevice* furnace() override;
  extraction::IStage* stage() override;
  extraction::IPatternRunner* pattern_runner() override;

  // ILaserDevice
  Result<void> fire_laser() override;
  Result<void> stop_laser() override;
  Result<bool> is_firing() override;
  Result<void> warmup() override;

  // IFurnaceDevice
  Result<double> read_temperature() override;
  Result<void> set_pid_parameters(double value) override;
  Result<void> drop_sample(std::string_view position) override;
  Result<void> dump_sample() override;

  // IStage
  Result<void> move_to_position(std::string_view position, bool autocenter) override;
  Result<void> set_axis(Axis axis, double value) override;
  Result<void> set_xy(double x, double y) override;
  Result<extraction::StagePosition> position() override;
  Result<bool> moving() override;
  Result<void> set_tray(std::string_view tray) override;
  std::vector<std::string> positions() const override { return settings_.positions; }

  // IPatternRunner
  Result<void> execute_pattern(std::string_view pattern) override;
  Result<bool> running() override;
  Result<void> stop_pattern() override;
  std::vector<std::string> patterns() const override { return settings_.patterns; }

 private:
  // Advances under `mutex_`; the caller reports the result after unlocking.
  std::optional<GasRelease> advance_locked();
  void report(std::optional<GasRelease> release);
  double target_locked() const;
  double released_fraction(double peak_c) const;
  bool known_position(std::string_view position) const;
  // Runs `f` under the lock after advancing, then reports.
  template <class F>
  auto locked(F f);

  SimExtractionSettings settings_;
  const Clock& clock_;
  GasReleaseHook hook_;

  mutable std::mutex mutex_;
  TimePoint last_;
  bool enabled_ = false;
  bool firing_ = false;
  double output_ = 0;
  extraction::ExtractUnits units_ = extraction::ExtractUnits::Percent;
  double temperature_;
  std::optional<std::string> sample_;
  std::map<std::string, double, std::less<>> peak_;      // per position, Celsius
  std::map<std::string, double, std::less<>> released_;  // per position, amount
  extraction::StagePosition at_;
  TimePoint move_end_{};
  std::optional<TimePoint> pattern_end_;
};

}  // namespace pychron::sim
