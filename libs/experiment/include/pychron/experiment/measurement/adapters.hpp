#pragma once

// Adapters from the real facades to the MeasurementEngine ports and to the
// plan resolvers:
//
//   SpectrometerPort        spectrometer::Spectrometer -> ISpectrometerPort
//   ExtractionLineValves    systems::ExtractionLine    -> IValvePort
//   ScriptMeasurementHook   scripting::IScriptHost     -> IMeasurementHook
//   SystemConfigAliases     extraction_line.toml [aliases] -> plan::IAliasResolver
//   SpectrometerCatalog     spectrometer.toml, weights, tables -> plan::ISpectrometerCatalog
//   InstrumentMetrics       spectrometer + line + devices  -> MetricContext for conditionals
//   SpectrometerPeakCenter  jobs::run_peak_center          -> IPeakCenterPort

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <set>
#include <string_view>
#include <vector>

#include "pychron/core/config/system_config.hpp"
#include "pychron/experiment/conditionals/evaluator.hpp"
#include "pychron/experiment/measurement/engine.hpp"
#include "pychron/experiment/measurement/ports.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "pychron/scripting/script.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/systems/extraction_line.hpp"
#include "pychron/systems/jobs/job_runner.hpp"
#include "pychron/systems/jobs/peak_center.hpp"
#include "pychron/systems/spectrometer/config.hpp"
#include "pychron/systems/spectrometer/data_dir.hpp"
#include "pychron/systems/spectrometer/spectrometer.hpp"

namespace pychron::experiment::measurement {

// position() puts a label or a mass on a detector (Spectrometer::position),
// a raw value through the move protocol (move_native) or an HV value through
// the source (position_hv), each with no settle; acquisition runs the
// facade's AcquisitionEngine and reads its IntensityStream. A running
// acquisition (a live readout) is restarted with the collection's
// integration time. with_table() keeps the facade's TableScope until the
// next with_table() or the port's end, which restores the table that was
// current before. The spectrometer outlives the port.
class SpectrometerPort final : public ISpectrometerPort {
 public:
  explicit SpectrometerPort(spectrometer::Spectrometer& spectrometer) : spec_(spectrometer) {}

  Result<void> position(const plan::At& target) override;
  Result<void> protect(const std::string& detector, bool on) override;
  Result<double> set_deflection(const std::string& detector, double value) override;
  Result<double> deflection(const std::string& detector) override;
  Result<void> with_table(const std::string& name) override;
  Result<void> set_gain(const std::string& detector, double gain) override;
  Result<void> set_hv(double volts) override;
  Result<void> set_cdd_voltage(double volts) override;
  Result<void> set_param(const std::string& name, double value) override;
  Result<void> blank_beam(bool on) override;
  Result<void> apply_optics(const std::string& name) override;
  Result<void> set_source_output(bool on) override;
  Result<void> start_acquisition(pychron::Duration integration) override;
  Result<std::optional<spectrometer::Reading>> next_reading(pychron::Duration timeout) override;
  void stop_acquisition() override;

 private:
  spectrometer::Spectrometer& spec_;
  std::optional<spectrometer::Spectrometer::TableScope> table_;  // declared after spec_: released first
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

// What a plan is checked against: the detectors of spectrometer.toml (with
// their isotope and whether their deflection can be set), the molecular
// weights, the names of the field tables loaded with the spectrometer and
// how the magnet and source can position. No optics presets yet: no driver
// applies one (plan v2, phase 8).
class SpectrometerCatalog final : public plan::ISpectrometerCatalog {
 public:
  SpectrometerCatalog(const spectrometer::cfg::SpectrometerConfig& config,
                      spectrometer::cfg::MolecularWeights weights, std::vector<std::string> table_names);
  bool has_detector(std::string_view name) const override;
  std::optional<std::string> isotope_of(std::string_view detector) const override;
  std::optional<double> mass_of(std::string_view label) const override;
  bool supports_deflection(std::string_view detector) const override;
  bool has_optics(std::string_view name) const override;
  bool has_table(std::string_view name) const override;
  bool positions_by_dac() const override;
  bool positions_by_hv() const override;

 private:
  std::map<std::string, std::string, std::less<>> detectors_;  // name -> configured isotope
  std::set<std::string, std::less<>> deflectable_;
  spectrometer::cfg::MolecularWeights weights_;  // label -> mass (amu)
  std::set<std::string, std::less<>> tables_;
  bool by_dac_ = false;
  bool by_hv_ = false;
};

// Each detector's large-beam and mode timing, for EngineContext::timing.
std::map<std::string, DetectorTiming> detector_timing(const spectrometer::cfg::SpectrometerConfig& config);

// The names of the field tables `data` loaded (its `tables`), for a catalog.
std::vector<std::string> table_names(const spectrometer::cfg::SpectrometerData& data);

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
