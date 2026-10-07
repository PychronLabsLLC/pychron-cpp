#include "pychron/experiment/measurement/adapters.hpp"

#include <cstdint>
#include <memory>
#include <utility>
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
