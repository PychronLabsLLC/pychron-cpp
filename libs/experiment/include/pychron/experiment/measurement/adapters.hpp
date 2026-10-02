#pragma once

// Adapters from the real facades to the MeasurementEngine ports and to the
// plan resolvers:
//
//   SpectrometerPort        spectrometer::Spectrometer -> ISpectrometerPort
//   ExtractionLineValves    systems::ExtractionLine    -> IValvePort
//   ScriptMeasurementHook   scripting::IScriptHost     -> IMeasurementHook
//   SystemConfigAliases     extraction_line.toml [aliases] -> plan::IAliasResolver
//   SpectrometerCatalog     spectrometer.toml detectors    -> plan::ISpectrometerCatalog
//   InstrumentMetrics       spectrometer + line + devices  -> MetricContext for conditionals
//   SpectrometerPeakCenter  jobs::run_peak_center          -> IPeakCenterPort

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "pychron/core/config/system_config.hpp"
#include "pychron/experiment/conditionals/evaluator.hpp"
#include "pychron/experiment/measurement/ports.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "pychron/scripting/script.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/jobs/job_runner.hpp"
#include "pychron/systems/jobs/peak_center.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::experiment::measurement {

// position() targets an isotope (or a mass for baselines) on a detector with
// no settle; acquisition runs the facade's AcquisitionEngine and reads its
// IntensityStream. A running acquisition (a live readout) is restarted with
// the collection's integration time.
class SpectrometerPort final : public ISpectrometerPort {
 public:
  explicit SpectrometerPort(spectrometer::Spectrometer& spectrometer) : spec_(spectrometer) {}

  Result<void> position(const plan::HopTarget& target) override;
  Result<void> protect(const std::string& detector, bool on) override;
  Result<void> start_acquisition(pychron::Duration integration) override;
  Result<std::optional<spectrometer::Reading>> next_reading(pychron::Duration timeout) override;
  void stop_acquisition() override;

 private:
  spectrometer::Spectrometer& spec_;
};

// Valve actuation through the line, so interlocks and SwitchManager events apply.
class ExtractionLineValves final : public IValvePort {
 public:
  explicit ExtractionLineValves(systems::ExtractionLine& line, std::string actor = "measurement")
      : line_(line), actor_(std::move(actor)) {}

  Result<void> open(const std::string& valve) override;
  Result<void> close(const std::string& valve) override;

 private:
  systems::ExtractionLine& line_;
  std::string actor_;
};

// Runs a measurement hook script on a script host. `env` is copied per call
// with `measurement` pointed at the engine's API.
class ScriptMeasurementHook final : public IMeasurementHook {
 public:
  ScriptMeasurementHook(scripting::IScriptHost& host, scripting::Script script, scripting::ScriptEnvironment env = {})
      : host_(host), script_(std::move(script)), env_(std::move(env)) {}

  Result<void> call(std::string_view entry, scripting::IMeasurementApi& api, scripting::CancelToken& token,
                    const scripting::ValueMap& args = {}) override;

 private:
  scripting::IScriptHost& host_;
  scripting::Script script_;
  scripting::ScriptEnvironment env_;
};

class SystemConfigAliases final : public plan::IAliasResolver {
 public:
  explicit SystemConfigAliases(const config::SystemConfig& config);
  std::optional<ParamValue> resolve_alias(std::string_view key) const override;

 private:
  std::map<std::string, ParamValue, std::less<>> aliases_;
};

class SpectrometerCatalog final : public plan::ISpectrometerCatalog {
 public:
  explicit SpectrometerCatalog(const spectrometer::cfg::SpectrometerConfig& config);
  bool has_detector(std::string_view name) const override;

 private:
  std::set<std::string, std::less<>> detectors_;
};

// The peak-center job for the MeasurementEngine. The request's config name
// selects one of `configs` ("default" falls back to the built-in defaults);
// the request's isotope and detector override the config's. With a JobRunner
// the job goes through it (the one-job-per-spectrometer interlock, JobStarted
// / JobFinished events); otherwise it runs directly. A cancel of the run's
// token cancels the job.
class SpectrometerPeakCenter final : public IPeakCenterPort {
 public:
  SpectrometerPeakCenter(spectrometer::Spectrometer& spectrometer,
                         std::map<std::string, jobs::PeakCenterConfig> configs = {}, jobs::JobRunner* runner = nullptr,
                         jobs::PeakCenterOptions options = {})
      : spec_(spectrometer), configs_(std::move(configs)), runner_(runner), options_(std::move(options)) {}

  Result<PeakCenterReport> peak_center(const PeakCenterRequest& request, scripting::CancelToken& token) override;

  // Full result of the last peak center (points, shapes), for display.
  std::optional<jobs::PeakCenterResult> last() const;

 private:
  Result<jobs::PeakCenterConfig> config_for(const PeakCenterRequest& request) const;

  spectrometer::Spectrometer& spec_;
  std::map<std::string, jobs::PeakCenterConfig> configs_;
  jobs::JobRunner* runner_;
  jobs::PeakCenterOptions options_;
  mutable std::mutex mutex_;
  std::optional<jobs::PeakCenterResult> last_;
};

// Instrument metrics for conditionals (conditionals spec section 5):
//   DET.deflection   Spectrometer::detector_state(DET).deflection
//   DET.inactive     1 when the detector is not active, else 0
//   gauge.G.pressure the line's latest recorded pressure of G, else a fresh read
//   device.NAME      the injected device reader (motors, resources, ...)
// Any pointer may be null; those metrics are then unavailable.
class InstrumentMetrics final : public MetricContext {
 public:
  using DeviceReader = std::function<Result<double>(std::string_view)>;
  InstrumentMetrics(spectrometer::Spectrometer* spectrometer, systems::ExtractionLine* line, DeviceReader devices = {})
      : spec_(spectrometer), line_(line), devices_(std::move(devices)) {}

  std::optional<std::vector<double>> series(const MetricRef& m) const override;
  std::optional<double> scalar(const MetricRef& m) const override;
  std::optional<double> elapsed() const override { return std::nullopt; }

 private:
  spectrometer::Spectrometer* spec_;
  systems::ExtractionLine* line_;
  DeviceReader devices_;
};

}  // namespace pychron::experiment::measurement
