#include "pychron/sim/spectrometer/beam_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>

#include "pychron/sim/keyed_noise.hpp"

namespace pychron::sim {

using spectrometer::DetectorKind;
using spectrometer::ParamId;
using spectrometer::SourceParam;

namespace {

double seconds(Duration d) { return std::chrono::duration<double>(d).count(); }

bool is_counter(DetectorKind kind) {
  return kind == DetectorKind::Counter || kind == DetectorKind::Cdd || kind == DetectorKind::Atona;
}

Error unknown_detector(std::string_view name) {
  return Error{ErrorKind::Config, "unknown detector '" + std::string(name) + "'", {}};
}

}  // namespace

std::vector<BeamGas> default_argon_gas() {
  return {
      {"Ar36", 35.96755, 3e3, 0.0},
      {"Ar37", 36.96678, 1e3, 0.0},
      {"Ar38", 37.96273, 1e3, 0.0},
      {"Ar39", 38.96431, 1e4, 0.0},
      {"Ar40", 39.96238, 1e6, 0.0},
  };
}

double default_table_value(double mass, const std::string&) { return mass / 5.0; }

DetectorKind infer_detector_kind(std::string_view name) {
  if (name == "CDD" || name == "EM" || name.starts_with("IC") || name.starts_with("counter")) {
    return DetectorKind::Counter;
  }
  return DetectorKind::Faraday;
}

BeamModel::BeamModel(const Clock& clock, BeamSettings settings)
    : clock_(clock), settings_(std::move(settings)), built_(clock.now()), t0_(built_) {
  if (settings_.gas.empty()) settings_.gas = default_argon_gas();
  if (!settings_.table_value) settings_.table_value = default_table_value;
  hv_ = settings_.nominal_hv;
  params_[ParamId{SourceParam::HV}] = hv_;
  params_[ParamId{SourceParam::TrapCurrent}] = settings_.nominal_trap_current;
  params_[ParamId{SourceParam::ExtractionFocus}] = settings_.extraction_focus_optimum;
}

const BeamDetector* BeamModel::find_locked(std::string_view name) const {
  auto it = std::ranges::find(detectors_, name, &BeamDetector::name);
  return it == detectors_.end() ? nullptr : &*it;
}

BeamDetector* BeamModel::find_locked(std::string_view name) {
  return const_cast<BeamDetector*>(std::as_const(*this).find_locked(name));
}

void BeamModel::add_detector(BeamDetector detector) {
  std::scoped_lock lock(mutex_);
  if (auto* existing = find_locked(detector.name)) {
    *existing = std::move(detector);
  } else {
    detectors_.push_back(std::move(detector));
  }
}

void BeamModel::ensure_detector(std::string_view name) {
  std::scoped_lock lock(mutex_);
  if (find_locked(name) != nullptr) return;
  BeamDetector d;
  d.name = std::string(name);
  d.kind = infer_detector_kind(name);
  detectors_.push_back(std::move(d));
}

bool BeamModel::has_detector(std::string_view name) const {
  std::scoped_lock lock(mutex_);
  return find_locked(name) != nullptr;
}

std::vector<std::string> BeamModel::detector_names() const {
  std::scoped_lock lock(mutex_);
  std::vector<std::string> names;
  for (const auto& d : detectors_) names.push_back(d.name);
  return names;
}

Result<BeamDetector> BeamModel::detector(std::string_view name) const {
  std::scoped_lock lock(mutex_);
  const auto* d = find_locked(name);
  if (d == nullptr) return fail(unknown_detector(name));
  return *d;
}

void BeamModel::set_gas(std::vector<BeamGas> gas) {
  std::scoped_lock lock(mutex_);
  settings_.gas = std::move(gas);
  t0_ = clock_.now();
}

void BeamModel::set_gas_provider(std::function<std::vector<BeamGas>(TimePoint)> provider) {
  std::scoped_lock lock(mutex_);
  settings_.gas_at = std::move(provider);
}

void BeamModel::set_magnet(double value) {
  std::scoped_lock lock(mutex_);
  magnet_ = value;
}

double BeamModel::magnet() const {
  std::scoped_lock lock(mutex_);
  return magnet_;
}

void BeamModel::set_hv(double volts) {
  std::scoped_lock lock(mutex_);
  hv_ = volts;
  params_[ParamId{SourceParam::HV}] = volts;
}

double BeamModel::hv() const {
  std::scoped_lock lock(mutex_);
  return hv_;
}

void BeamModel::set_source_param(const ParamId& id, double value) {
  if (id == ParamId{SourceParam::HV}) return set_hv(value);
  std::scoped_lock lock(mutex_);
  params_[id] = value;
}

double BeamModel::param_locked(const ParamId& id) const {
  auto it = params_.find(id);
  return it == params_.end() ? 0.0 : it->second;
}

double BeamModel::source_param(const ParamId& id) const {
  std::scoped_lock lock(mutex_);
  return param_locked(id);
}

Result<void> BeamModel::set_deflection(std::string_view det, double value) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  d->deflection = value;
  return {};
}

Result<void> BeamModel::set_gain(std::string_view det, double value) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  d->gain = value;
  return {};
}

Result<void> BeamModel::set_cdd_voltage(std::string_view det, double volts) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  d->cdd_voltage = volts;
  return {};
}

Result<void> BeamModel::set_baseline(std::string_view det, double baseline, double drift_per_h) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  if (!std::isfinite(baseline) || !std::isfinite(drift_per_h)) {
    return fail(ErrorKind::Config, "baseline of detector '" + std::string(det) + "' is not a finite number");
  }
  d->baseline = baseline;
  d->baseline_drift_per_h = drift_per_h;
  return {};
}

Result<void> BeamModel::protect(std::string_view det, bool on) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  d->protect = on;
  return {};
}

void BeamModel::blank(bool on) {
  std::scoped_lock lock(mutex_);
  blank_ = on;
}

bool BeamModel::blanked() const {
  std::scoped_lock lock(mutex_);
  return blank_;
}

bool BeamModel::overloaded(std::string_view det) const {
  std::scoped_lock lock(mutex_);
  const auto* d = find_locked(det);
  return d != nullptr && d->overloaded;
}

Result<void> BeamModel::clear_overload(std::string_view det) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  d->overloaded = false;
  return {};
}

double BeamModel::shift_locked(const BeamDetector& d) const {
  double poly = 0.0;
  double power = 1.0;
  for (double c : d.deflection_correction) {
    poly += c * power;
    power *= d.deflection;
  }
  double symmetry = param_locked(ParamId{SourceParam::YSymmetry}) + param_locked(ParamId{SourceParam::ZSymmetry});
  return d.deflection_sign * poly + d.offset + settings_.symmetry_shift_per_unit * symmetry;
}

double BeamModel::center_locked(const BeamDetector& d, double mass) const {
  return settings_.table_value(mass, d.name) * std::sqrt(hv_ / settings_.nominal_hv) + shift_locked(d);
}

double BeamModel::sensitivity_locked(const BeamDetector& d) const {
  double s = d.sensitivity * d.gain;
  s *= param_locked(ParamId{SourceParam::TrapCurrent}) / settings_.nominal_trap_current;
  double focus = (param_locked(ParamId{SourceParam::ExtractionFocus}) - settings_.extraction_focus_optimum) /
                 settings_.extraction_focus_width;
  s *= std::exp(-0.5 * focus * focus);
  if (is_counter(d.kind)) {
    s /= 1.0 + std::exp(-(d.cdd_voltage - settings_.cdd_plateau_center) / settings_.cdd_plateau_width);
  }
  return s;
}

double BeamModel::shape_locked(double magnet, double center) const {
  double dist = std::abs(magnet - center);
  if (dist <= settings_.flat_half_width) return 1.0;
  double edge = dist - settings_.flat_half_width;
  return edge >= settings_.edge_width ? 0.0 : 1.0 - edge / settings_.edge_width;
}

double BeamModel::true_signal_locked(const BeamDetector& d, TimePoint t) const {
  if (blank_ || d.protect) return 0.0;
  double sum = 0.0;
  if (settings_.gas_at) {
    // The provider answers for `t` itself: its rates are not applied.
    for (const auto& g : settings_.gas_at(t)) {
      sum += g.abundance * shape_locked(magnet_, center_locked(d, g.mass));
    }
  } else {
    double elapsed = seconds(t - t0_);
    for (const auto& g : settings_.gas) {
      double abundance = g.abundance * std::exp(g.rate_per_s * elapsed);
      sum += abundance * shape_locked(magnet_, center_locked(d, g.mass));
    }
  }
  return sum * sensitivity_locked(d);
}

double BeamModel::baseline_locked(const BeamDetector& d, TimePoint t) const {
  return d.baseline + d.baseline_drift_per_h * seconds(t - built_) / 3600.0;
}

Result<double> BeamModel::peak_center(std::string_view det, std::string_view isotope) const {
  std::scoped_lock lock(mutex_);
  const auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));
  if (settings_.gas_at) {
    for (const auto& g : settings_.gas_at(clock_.now())) {
      if (g.isotope == isotope) return center_locked(*d, g.mass);
    }
  }
  for (const auto& g : settings_.gas) {
    if (g.isotope == isotope) return center_locked(*d, g.mass);
  }
  return fail(ErrorKind::Config, "unknown isotope '" + std::string(isotope) + "'");
}

Result<BeamIntensity> BeamModel::intensity(std::string_view det, Duration gate) {
  return intensity(det, clock_.now(), gate);
}

Result<BeamIntensity> BeamModel::intensity(std::string_view det, TimePoint t, Duration gate) {
  std::scoped_lock lock(mutex_);
  auto* d = find_locked(det);
  if (d == nullptr) return fail(unknown_detector(det));

  double signal = true_signal_locked(*d, t);
  double baseline = baseline_locked(*d, t);
  // The noise of this reading: of this detector at this instant, whoever
  // else read what before (keyed_noise.hpp).
  auto tick = std::chrono::duration_cast<std::chrono::nanoseconds>(t - built_).count();
  BeamIntensity out;
  if (is_counter(d->kind)) {
    if (!d->protect && signal > d->overload_threshold) {
      d->overloaded = true;
      out.overloaded = true;
    }
    double tau = d->dead_time_ns * 1e-9;
    double measured = signal / (1.0 + signal * tau);
    double g = std::max(seconds(gate), 1e-9);
    // Dark counts come after the dead time and never overload.
    double mean = std::clamp(measured * g + baseline * g, 0.0, 1e12);
    double counts = 0.0;
    if (mean > 0.0) {
      std::mt19937_64 rng(keyed_bits(settings_.seed, d->name, tick));
      counts = static_cast<double>(std::poisson_distribution<std::int64_t>(mean)(rng));
    }
    out.value = counts / g;
  } else {
    double sigma = d->noise_floor + d->noise_rel * std::abs(signal);
    out.value = signal + baseline + sigma * keyed_gauss(settings_.seed, d->name, tick);
  }
  if (out.value >= d->saturation) {
    out.value = d->saturation;
    out.saturated = true;
  }
  return out;
}

BeamModelRegistry& BeamModelRegistry::global() {
  static BeamModelRegistry registry;
  return registry;
}

std::shared_ptr<BeamModel> BeamModelRegistry::acquire(const std::string& name, const Clock& clock) {
  std::scoped_lock lock(mutex_);
  auto& slot = models_[name];
  if (!slot) slot = std::make_shared<BeamModel>(clock);
  return slot;
}

void BeamModelRegistry::set(const std::string& name, std::shared_ptr<BeamModel> model) {
  std::scoped_lock lock(mutex_);
  models_[name] = std::move(model);
}

void BeamModelRegistry::clear() {
  std::scoped_lock lock(mutex_);
  models_.clear();
}

}  // namespace pychron::sim
