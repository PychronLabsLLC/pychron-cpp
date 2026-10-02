#include "pychron/experiment/measurement/adapters.hpp"

#include <chrono>
#include <variant>

namespace pychron::experiment::measurement {

Result<void> SpectrometerPort::position(const plan::HopTarget& target) {
  spectrometer::PositionTarget t;
  if (target.mass) {
    t.target = spectrometer::Mass{*target.mass};
  } else {
    t.target = spectrometer::Isotope{target.isotope};
  }
  t.on = target.detector;
  spectrometer::PositionOptions options;
  options.settle = pychron::Duration::zero();
  auto r = spec_.position(t, options);
  if (!r) return fail(r.error());
  return {};
}

Result<void> SpectrometerPort::protect(const std::string& detector, bool on) { return spec_.protect(detector, on); }

Result<void> SpectrometerPort::start_acquisition(pychron::Duration integration) {
  auto& engine = spec_.acquisition();
  if (engine.running()) engine.stop();
  return engine.start(integration);
}

Result<std::optional<spectrometer::Reading>> SpectrometerPort::next_reading(pychron::Duration timeout) {
  return spec_.acquisition().stream()->next(timeout);
}

void SpectrometerPort::stop_acquisition() { spec_.acquisition().stop(); }

Result<void> ExtractionLineValves::open(const std::string& valve) {
  return line_.actuate(valve, systems::SwitchOp::Open, actor_);
}

Result<void> ExtractionLineValves::close(const std::string& valve) {
  return line_.actuate(valve, systems::SwitchOp::Close, actor_);
}

Result<void> ScriptMeasurementHook::call(std::string_view entry, scripting::IMeasurementApi& api,
                                         scripting::CancelToken& token, const scripting::ValueMap& args) {
  scripting::ScriptEnvironment env = env_;
  env.measurement = &api;
  auto r = host_.call_hook(script_, entry, args, env, token);
  if (!r) return fail(r.error());
  return {};
}

SystemConfigAliases::SystemConfigAliases(const config::SystemConfig& config) {
  for (const auto& [key, a] : config.aliases) {
    std::visit([&, &k = key](const auto& v) { aliases_.emplace(k, ParamValue{v}); }, a.value);
  }
}

std::optional<ParamValue> SystemConfigAliases::resolve_alias(std::string_view key) const {
  auto it = aliases_.find(key);
  if (it == aliases_.end()) return std::nullopt;
  return it->second;
}

SpectrometerCatalog::SpectrometerCatalog(const spectrometer::cfg::SpectrometerConfig& config) {
  for (const auto& d : config.detectors) detectors_.insert(d.name);
}

bool SpectrometerCatalog::has_detector(std::string_view name) const { return detectors_.contains(name); }

std::optional<std::vector<double>> InstrumentMetrics::series(const MetricRef& m) const {
  if (auto v = scalar(m)) return std::vector<double>{*v};
  return std::nullopt;
}

std::optional<double> InstrumentMetrics::scalar(const MetricRef& m) const {
  using K = MetricRef::Kind;
  switch (m.kind) {
    case K::DetectorField: {
      if (spec_ == nullptr || m.field == "intensity") return std::nullopt;
      auto st = spec_->detector_state(m.a);
      if (!st) return std::nullopt;
      if (m.field == "inactive") return st->active ? 0.0 : 1.0;
      return st->deflection;
    }
    case K::Gauge: {
      if (line_ == nullptr) return std::nullopt;
      const auto snap = line_->snapshot();
      if (auto it = snap.pressures.find(m.a); it != snap.pressures.end()) return it->second;
      auto r = line_->read_gauge(m.a);
      if (!r) return std::nullopt;
      return *r;
    }
    case K::Device: {
      if (!devices_) return std::nullopt;
      auto r = devices_(m.a);
      if (!r) return std::nullopt;
      return *r;
    }
    default: return std::nullopt;
  }
}

}  // namespace pychron::experiment::measurement
