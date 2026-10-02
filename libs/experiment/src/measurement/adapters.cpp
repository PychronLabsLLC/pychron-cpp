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
                                         scripting::CancelToken& token) {
  scripting::ScriptEnvironment env = env_;
  env.measurement = &api;
  auto r = host_.call_hook(script_, entry, {}, env, token);
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

}  // namespace pychron::experiment::measurement
