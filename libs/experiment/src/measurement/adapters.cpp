#include "pychron/experiment/measurement/adapters.hpp"

#include <cstdint>
#include <iterator>
#include <memory>
#include <type_traits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace pychron::experiment::measurement {

namespace {

spectrometer::PositionOptions no_settle() {
  spectrometer::PositionOptions options;
  options.settle = pychron::Duration::zero();
  return options;
}

}  // namespace

Result<void> SpectrometerPort::position(const plan::At& target) {
  return std::visit(
      [&](const auto& at) -> Result<void> {
        using T = std::decay_t<decltype(at)>;
        if constexpr (std::is_same_v<T, plan::AtLabel>) {
          auto r = spec_.position(spectrometer::PositionTarget{spectrometer::Isotope{at.label}, at.detector}, no_settle());
          if (!r) return fail(r.error());
        } else if constexpr (std::is_same_v<T, plan::AtMass>) {
          auto r = spec_.position(spectrometer::PositionTarget{spectrometer::Mass{at.mass}, at.detector}, no_settle());
          if (!r) return fail(r.error());
        } else if constexpr (std::is_same_v<T, plan::AtDac>) {
          auto r = spec_.move_native(at.dac, no_settle());
          if (!r) return fail(r.error());
        } else {
          auto r = spec_.position_hv(spectrometer::PositionTarget{spectrometer::NativeUnits{at.hv}, at.detector},
                                     no_settle());
          if (!r) return fail(r.error());
        }
        return {};
      },
      target);
}

Result<void> SpectrometerPort::protect(const std::string& detector, bool on) { return spec_.protect(detector, on); }

Result<double> SpectrometerPort::set_deflection(const std::string& detector, double value) {
  return spec_.set_deflection(detector, value);
}

Result<double> SpectrometerPort::deflection(const std::string& detector) { return spec_.read_deflection(detector); }

Result<void> SpectrometerPort::with_table(const std::string& name) {
  table_.reset();  // back to the table that was current before the last call
  if (name.empty()) return {};
  auto scope = spec_.with_table(name);
  if (!scope) return fail(scope.error());
  table_.emplace(std::move(*scope));
  return {};
}

Result<void> SpectrometerPort::set_gain(const std::string& detector, double gain) {
  return spec_.set_gain(detector, gain);
}

Result<void> SpectrometerPort::set_hv(double volts) { return spec_.set_hv(volts); }

Result<void> SpectrometerPort::set_cdd_voltage(double volts) {
  bool any = false;
  for (const auto& d : spec_.config().detectors) {
    if (d.kind != spectrometer::cfg::DetectorKind::Cdd) continue;
    any = true;
    if (auto r = spec_.set_cdd_voltage(d.name, volts); !r) return r;
  }
  if (!any) return fail(ErrorKind::Config, "cdd_voltage: the spectrometer has no CDD");
  return {};
}

Result<void> SpectrometerPort::set_param(const std::string& name, double value) {
  auto id = spectrometer::parse_param_id(name);
  if (!id) {
    // Not a registry name: the source's own (vendor) name for one.
    auto specs = spec_.source_params();
    if (!specs) return fail(specs.error());
    const auto* spec = spectrometer::find_vendor(*specs, name);
    if (spec == nullptr) return fail(id.error());
    id = spec->id;
  }
  return spec_.set_param(*id, value);
}

Result<void> SpectrometerPort::blank_beam(bool on) { return spec_.blank_beam(on); }

Result<void> SpectrometerPort::apply_optics(const std::string& name) {
  return fail(ErrorKind::Config, "optics '" + name + "': not supported by this spectrometer");
}

Result<void> SpectrometerPort::set_source_output(bool /*on*/) {
  return fail(ErrorKind::Config, "source output: not supported by this spectrometer");
}

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

SpectrometerCatalog::SpectrometerCatalog(const spectrometer::cfg::SpectrometerConfig& config,
                                         spectrometer::cfg::MolecularWeights weights,
                                         std::vector<std::string> table_names)
    : weights_(std::move(weights)),
      tables_(std::make_move_iterator(table_names.begin()), std::make_move_iterator(table_names.end())),
      // A raw value goes to the positioner only when its native axis is a DAC;
      // an HV position needs a source to set it on.
      by_dac_(!config.magnet.positioner.empty() && config.magnet.native_axis == spectrometer::cfg::Axis::Dac),
      by_hv_(!config.source.driver.empty()) {
  for (const auto& d : config.detectors) {
    detectors_.emplace(d.name, d.isotope);
    if (d.deflection && d.deflection->control) deflectable_.insert(d.name);
  }
}

std::map<std::string, DetectorTiming> detector_timing(const spectrometer::cfg::SpectrometerConfig& config) {
  std::map<std::string, DetectorTiming> out;
  for (const auto& d : config.detectors)
    out.emplace(d.name, DetectorTiming{d.large_beam_threshold, d.settle_after_large_beam_s, d.collector, d.mode_switch_s});
  return out;
}

bool SpectrometerCatalog::has_detector(std::string_view name) const { return detectors_.contains(name); }

std::optional<std::string> SpectrometerCatalog::isotope_of(std::string_view detector) const {
  const auto it = detectors_.find(detector);
  if (it == detectors_.end() || it->second.empty()) return std::nullopt;
  return it->second;
}

std::optional<double> SpectrometerCatalog::mass_of(std::string_view label) const {
  const auto it = weights_.find(label);
  if (it == weights_.end()) return std::nullopt;
  return it->second;
}

bool SpectrometerCatalog::supports_deflection(std::string_view detector) const {
  return deflectable_.contains(detector);
}

bool SpectrometerCatalog::has_optics(std::string_view /*name*/) const { return false; }

bool SpectrometerCatalog::has_table(std::string_view name) const { return tables_.contains(name); }

bool SpectrometerCatalog::positions_by_dac() const { return by_dac_; }

bool SpectrometerCatalog::positions_by_hv() const { return by_hv_; }

std::vector<std::string> table_names(const spectrometer::cfg::SpectrometerData& data) {
  std::vector<std::string> out;
  out.reserve(data.tables.size());
  for (const auto& kv : data.tables) out.push_back(kv.first);
  return out;
}

Result<jobs::PeakCenterConfig> SpectrometerPeakCenter::config_for(const PeakCenterRequest& request) const {
  const std::string name = request.config.empty() ? "default" : request.config;
  jobs::PeakCenterConfig cfg;
  if (auto it = configs_.find(name); it != configs_.end()) {
    cfg = it->second;
  } else if (name != "default") {
    return fail(ErrorKind::Config, "unknown peak center config '" + name + "'");
  }
  if (!request.isotope.empty()) cfg.isotope = request.isotope;
  if (!request.detector.empty()) cfg.detector = request.detector;
  return cfg;
}

Result<PeakCenterReport> SpectrometerPeakCenter::peak_center(const PeakCenterRequest& request,
                                                            scripting::CancelToken& token) {
  auto cfg = config_for(request);
  if (!cfg) return fail(cfg.error());

  // Bridge the run's token to the job's: a cancel of the run is made on the
  // job by the thread that requests it, for as long as the job runs. Shared:
  // the runner keeps the job's body, which holds it, in its history.
  const auto job_token = std::make_shared<jobs::CancelToken>();
  struct Bridge {
    scripting::CancelToken& token;
    std::uint64_t id;
    // Waits for a call in progress: the callback refers to this frame.
    ~Bridge() { token.remove_on_cancel(id); }
  } bridge{token, token.add_on_cancel([&] { job_token->cancel(); })};
  Result<jobs::PeakCenterResult> result = fail(ErrorKind::Config, "peak center did not run");
  if (runner_ != nullptr) {
    // The runner gives the job a token of its own: the job links job_token
    // to it for as long as it runs, so this call cancels its own job and no
    // other on the runner. A cancel made before the job starts is passed on
    // as the link is made.
    auto spec = jobs::peak_center_job(*cfg, options_);
    spec.body = [job_token, body = std::move(spec.body)](jobs::JobContext& ctx) -> Result<std::any> {
      auto link = job_token->on_cancel([&ctx] { ctx.cancel.cancel(); });
      if (ctx.cancel.cancelled()) return fail(ErrorKind::Cancelled, "peak center cancelled", "peak_center");
      return body(ctx);
    };
    auto job = runner_->run(std::move(spec));
    if (!job) {
      result = fail(job.error());
    } else if (job->state != jobs::JobState::Succeeded) {
      result = fail(job->error.value_or(Error{ErrorKind::Io, "peak center job failed", {}}));
    } else if (const auto* r = std::any_cast<jobs::PeakCenterResult>(&job->result)) {
      result = *r;
    }
  } else {
    jobs::Progress progress;
    result = jobs::run_peak_center(spec_, *cfg, progress, *job_token, options_);
  }
  if (!result) return fail(result.error());

  {
    std::lock_guard lock(mutex_);
    last_ = *result;
  }
  PeakCenterReport report;
  report.request = request;
  report.ok = result->ok;
  report.center = result->center;
  report.message = result->ok ? "" : result->message;
  report.table_value = result->table_value;
  report.table_updated = result->table_updated;
  if (result->shape) report.resolution = result->shape->resolution;
  return report;
}

std::optional<jobs::PeakCenterResult> SpectrometerPeakCenter::last() const {
  std::lock_guard lock(mutex_);
  return last_;
}

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
