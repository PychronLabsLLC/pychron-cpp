#pragma once

// Adapters from the real facades to the MeasurementEngine ports and to the
// plan resolvers:
//
//   SpectrometerPort        spectrometer::Spectrometer -> ISpectrometerPort
//   ExtractionLineValves    systems::ExtractionLine    -> IValvePort
//   ScriptMeasurementHook   scripting::IScriptHost     -> IMeasurementHook
//   SystemConfigAliases     extraction_line.toml [aliases] -> plan::IAliasResolver
//   SpectrometerCatalog     spectrometer.toml detectors    -> plan::ISpectrometerCatalog

#include <map>
#include <set>
#include <string>
#include <string_view>

#include "pychron/core/config/system_config.hpp"
#include "pychron/experiment/measurement/ports.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"
#include "pychron/scripting/script.hpp"
#include "pychron/scripting/script_host.hpp"
#include "pychron/systems/extraction_line.hpp"
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

  Result<void> call(std::string_view entry, scripting::IMeasurementApi& api, scripting::CancelToken& token) override;

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

}  // namespace pychron::experiment::measurement
