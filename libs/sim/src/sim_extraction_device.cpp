#include "pychron/sim/sim_extraction_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pychron::sim {

using extraction::Capability;
using extraction::ExtractUnits;

namespace {

constexpr double kPositionSpacingMm = 2.0;

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

}  // namespace

SimExtractionDevice::SimExtractionDevice(SimExtractionSettings settings, const Clock& clock)
    : settings_(std::move(settings)),
      clock_(clock),
      last_(clock.now()),
      temperature_(settings_.ambient_c) {}

template <class F>
auto SimExtractionDevice::locked(F f) {
  std::unique_lock lock(mutex_);
  auto release = advance_locked();
  auto result = f();
  lock.unlock();
  report(std::move(release));
  return result;
}

void SimExtractionDevice::on_gas_release(GasReleaseHook hook) {
  std::lock_guard lock(mutex_);
  hook_ = std::move(hook);
}

void SimExtractionDevice::update() {
  locked([] { return 0; });
}

double SimExtractionDevice::temperature() {
  return locked([this] { return temperature_; });
}

double SimExtractionDevice::released(std::string_view position) const {
  std::lock_guard lock(mutex_);
  auto it = released_.find(position);
  return it == released_.end() ? 0.0 : it->second;
}

// --- model ------------------------------------------------------------------

double SimExtractionDevice::target_locked() const {
  bool heating = enabled_ && output_ > 0 &&
                 (!settings_.features.has(Capability::Laser) || firing_);
  if (!heating) return settings_.ambient_c;
  switch (units_) {
    case ExtractUnits::Celsius: return output_;
    case ExtractUnits::Percent:
      return settings_.ambient_c + settings_.c_per_watt * settings_.max_power_watts * output_ / 100.0;
    case ExtractUnits::Watts: return settings_.ambient_c + settings_.c_per_watt * output_;
  }
  return settings_.ambient_c;
}

double SimExtractionDevice::released_fraction(double peak_c) const {
  auto logistic = [this](double t) {
    return 1.0 / (1.0 + std::exp(-(t - settings_.release_c50) / settings_.release_width_c));
  };
  double floor = logistic(settings_.ambient_c);
  return std::clamp((logistic(peak_c) - floor) / (1.0 - floor), 0.0, 1.0);
}

std::optional<GasRelease> SimExtractionDevice::advance_locked() {
  auto now = clock_.now();
  if (now <= last_) return std::nullopt;
  double dt = seconds(now - last_);
  last_ = now;

  double start = temperature_;
  double target = target_locked();
  double tau = seconds(settings_.thermal_tau);
  temperature_ = tau > 0 ? target + (start - target) * std::exp(-dt / tau) : target;

  if (!sample_) return std::nullopt;
  // The approach is monotonic, so the interval's peak is at one of its ends.
  double& peak = peak_.try_emplace(*sample_, settings_.ambient_c).first->second;
  peak = std::max({peak, start, temperature_});
  double total = settings_.gas_per_position * released_fraction(peak);
  double& so_far = released_[*sample_];
  if (total <= so_far) return std::nullopt;
  GasRelease release{*sample_, total - so_far, temperature_, now};
  so_far = total;
  return release;
}

void SimExtractionDevice::report(std::optional<GasRelease> release) {
  if (!release) return;
  GasReleaseHook hook;
  {
    std::lock_guard lock(mutex_);
    hook = hook_;
  }
  if (hook) hook(*release);
}

bool SimExtractionDevice::known_position(std::string_view position) const {
  return std::find(settings_.positions.begin(), settings_.positions.end(), position) !=
         settings_.positions.end();
}

// --- IExtractionDevice ------------------------------------------------------

Result<void> SimExtractionDevice::enable() {
  return locked([this]() -> Result<void> {
    enabled_ = true;
    return {};
  });
}

Result<void> SimExtractionDevice::disable() {
  return locked([this]() -> Result<void> {
    enabled_ = false;
    firing_ = false;
    output_ = 0;
    return {};
  });
}

Result<bool> SimExtractionDevice::is_enabled() {
  return locked([this]() -> Result<bool> { return enabled_; });
}

bool SimExtractionDevice::supports(ExtractUnits units) const {
  return units != ExtractUnits::Celsius || settings_.features.has(Capability::Furnace);
}

Result<void> SimExtractionDevice::extract(double value, ExtractUnits units) {
  return locked([&]() -> Result<void> {
    const auto& name = settings_.name;
    if (!supports(units)) return fail(extraction::not_supported(to_string(units), name));
    if (!(value >= 0)) return fail(ErrorKind::Config, "output must be >= 0", name);
    if (units == ExtractUnits::Percent && value > 100)
      return fail(ErrorKind::Config, "percent output must be <= 100", name);
    if (!enabled_) return fail(ErrorKind::Interlock, "extract while disabled", name);
    output_ = value;
    units_ = units;
    return {};
  });
}

Result<void> SimExtractionDevice::end_extract() {
  return locked([this]() -> Result<void> {
    output_ = 0;
    firing_ = false;
    return {};
  });
}

Result<double> SimExtractionDevice::output() {
  return locked([this]() -> Result<double> { return output_; });
}

extraction::ILaserDevice* SimExtractionDevice::laser() {
  return settings_.features.has(Capability::Laser) ? this : nullptr;
}
extraction::IFurnaceDevice* SimExtractionDevice::furnace() {
  return settings_.features.has(Capability::Furnace) ? this : nullptr;
}
extraction::IStage* SimExtractionDevice::stage() {
  return settings_.features.has(Capability::Stage) ? this : nullptr;
}
extraction::IPatternRunner* SimExtractionDevice::pattern_runner() {
  return settings_.features.has(Capability::Pattern) ? this : nullptr;
}

// --- ILaserDevice -----------------------------------------------------------

Result<void> SimExtractionDevice::fire_laser() {
  return locked([this]() -> Result<void> {
    if (!enabled_) return fail(ErrorKind::Interlock, "fire while disabled", settings_.name);
    firing_ = true;
    return {};
  });
}

Result<void> SimExtractionDevice::stop_laser() {
  return locked([this]() -> Result<void> {
    firing_ = false;
    return {};
  });
}

Result<bool> SimExtractionDevice::is_firing() {
  return locked([this]() -> Result<bool> { return firing_; });
}

Result<void> SimExtractionDevice::warmup() { return {}; }

// --- IFurnaceDevice ---------------------------------------------------------

Result<double> SimExtractionDevice::read_temperature() {
  return locked([this]() -> Result<double> { return temperature_; });
}

Result<void> SimExtractionDevice::set_pid_parameters(double value) {
  if (!(value >= 0)) return fail(ErrorKind::Config, "pid output must be >= 0", settings_.name);
  return {};
}

Result<void> SimExtractionDevice::drop_sample(std::string_view position) {
  return locked([&]() -> Result<void> {
    if (!known_position(position))
      return fail(ErrorKind::Config, "unknown position " + std::string(position), settings_.name);
    // Dropped into the crucible: the sample starts at its temperature.
    sample_ = std::string(position);
    return {};
  });
}

Result<void> SimExtractionDevice::dump_sample() {
  return locked([this]() -> Result<void> {
    sample_.reset();
    return {};
  });
}

// --- IStage -----------------------------------------------------------------

Result<void> SimExtractionDevice::move_to_position(std::string_view position, bool) {
  return locked([&]() -> Result<void> {
    auto it = std::find(settings_.positions.begin(), settings_.positions.end(), position);
    if (it == settings_.positions.end())
      return fail(ErrorKind::Config, "unknown position " + std::string(position), settings_.name);
    auto index = static_cast<double>(it - settings_.positions.begin());
    at_.x = index * kPositionSpacingMm;
    at_.y = 0;
    move_end_ = clock_.now() + settings_.move_time;
    // A fresh spot under the beam starts cold.
    sample_ = *it;
    temperature_ = settings_.ambient_c;
    return {};
  });
}

Result<void> SimExtractionDevice::set_axis(Axis axis, double value) {
  return locked([&]() -> Result<void> {
    (axis == Axis::X ? at_.x : axis == Axis::Y ? at_.y : at_.z) = value;
    move_end_ = clock_.now() + settings_.move_time;
    return {};
  });
}

Result<void> SimExtractionDevice::set_xy(double x, double y) {
  return locked([&]() -> Result<void> {
    at_.x = x;
    at_.y = y;
    move_end_ = clock_.now() + settings_.move_time;
    return {};
  });
}

Result<extraction::StagePosition> SimExtractionDevice::position() {
  return locked([this]() -> Result<extraction::StagePosition> { return at_; });
}

Result<bool> SimExtractionDevice::moving() {
  return locked([this]() -> Result<bool> { return clock_.now() < move_end_; });
}

Result<void> SimExtractionDevice::set_tray(std::string_view tray) {
  if (std::find(settings_.trays.begin(), settings_.trays.end(), tray) == settings_.trays.end())
    return fail(ErrorKind::Config, "unknown tray " + std::string(tray), settings_.name);
  return {};
}

// --- IPatternRunner ---------------------------------------------------------

Result<void> SimExtractionDevice::execute_pattern(std::string_view pattern) {
  return locked([&]() -> Result<void> {
    if (std::find(settings_.patterns.begin(), settings_.patterns.end(), pattern) ==
        settings_.patterns.end())
      return fail(ErrorKind::Config, "unknown pattern " + std::string(pattern), settings_.name);
    pattern_end_ = clock_.now() + settings_.pattern_time;
    return {};
  });
}

Result<bool> SimExtractionDevice::running() {
  return locked([this]() -> Result<bool> { return pattern_end_ && clock_.now() < *pattern_end_; });
}

Result<void> SimExtractionDevice::stop_pattern() {
  return locked([this]() -> Result<void> {
    pattern_end_.reset();
    return {};
  });
}

}  // namespace pychron::sim
