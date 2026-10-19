#pragma once

// The hardware surface the MeasurementEngine uses (experiment spec 4.2):
// spectrometer primitives, extraction-line valves, the peak-center job and the
// measurement hook. Narrow interfaces so the engine runs against fakes in unit
// tests and against the real facades through adapters.hpp.

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/scripting/cancel_token.hpp"
#include "pychron/scripting/script.hpp"
#include "pychron/scripting/services.hpp"
#include "pychron/systems/spectrometer/acquisition.hpp"

namespace pychron::experiment::measurement {

class ISpectrometerPort {
 public:
  virtual ~ISpectrometerPort() = default;
  // Positions the beam: a label or a mass on a detector through the magnet,
  // a raw native value (`AtDac`) through the magnet's move protocol, or an
  // HV value on a detector (`AtHv`) through the source. No settle: the engine
  // settles itself so the wait is cancellable.
  virtual Result<void> position(const plan::At& target) = 0;
  virtual Result<void> protect(const std::string& detector, bool on) = 0;

  // Per-position and step settings (plan v2, spec 5.2 and 5.3). Deflection
  // answers the value actually set (clamped to the detector's limit);
  // deflection(detector) answers the current one, so the engine can restore
  // what a position changed.
  virtual Result<double> set_deflection(const std::string& detector, double value) = 0;
  virtual Result<double> deflection(const std::string& detector) = 0;
  // Selects field table `name` until the next call; an empty name goes back
  // to the spectrometer's configured current table.
  virtual Result<void> with_table(const std::string& name) = 0;
  virtual Result<void> set_gain(const std::string& detector, double gain) = 0;
  virtual Result<void> set_hv(double volts) = 0;
  // On every CDD the spectrometer has.
  virtual Result<void> set_cdd_voltage(double volts) = 0;
  // A source parameter by its registry name ("trap_current") or vendor name.
  virtual Result<void> set_param(const std::string& name, double value) = 0;
  virtual Result<void> blank_beam(bool on) = 0;
  // Optics preset and source output: no driver applies them yet (plan v2,
  // phase 8); the adapter answers a Config error.
  virtual Result<void> apply_optics(const std::string& name) = 0;
  virtual Result<void> set_source_output(bool on) = 0;

  // Integration has no setter: it is an argument of start_acquisition, and a
  // change of integration is the engine restarting acquisition.
  // Readings flow from start until stop; start discards anything queued.
  virtual Result<void> start_acquisition(pychron::Duration integration) = 0;
  // nullopt when nothing arrived within `timeout`.
  virtual Result<std::optional<spectrometer::Reading>> next_reading(pychron::Duration timeout) = 0;
  virtual void stop_acquisition() = 0;
};

class IValvePort {
 public:
  virtual ~IValvePort() = default;
  virtual Result<void> open(const std::string& valve) = 0;
  virtual Result<void> close(const std::string& valve) = 0;
};

struct PeakCenterRequest {
  std::string isotope;
  std::string detector;
  std::string config = "default";
  std::vector<std::string> extra_detectors;  // fitted too, for display
  // Non-empty: the center goes into this run's own table of that name (a
  // copy of the current table made at its first peak center), not into the
  // current table; never saved.
  std::string write_table;
};

struct PeakCenterReport {
  PeakCenterRequest request;
  bool ok = false;
  bool skipped = false;  // by the MeasurementEngine, under the plan's min_signal: the port was not called
  std::optional<double> center;  // native magnet value when ok
  std::string message;
  std::optional<double> table_value;  // the uncorrected center written to the field table
  bool table_updated = false;
  std::optional<double> resolution;
  TimePoint finished{};  // set by the MeasurementEngine
};

// The peak-center job (spectrometer spec, peak_center unit). A failed peak
// center is a report with ok = false, not an error; errors are reserved for
// cancellation and hardware faults.
class IPeakCenterPort {
 public:
  virtual ~IPeakCenterPort() = default;
  virtual Result<PeakCenterReport> peak_center(const PeakCenterRequest& request,
                                               scripting::CancelToken& token) = 0;
  // The run is over: its own tables (write_table) are forgotten.
  virtual void drop_run_tables() {}
};

// A gas's prepare script (staged release, spec 11.2): the Run runs it as an
// extraction script on the run's token. `variables` as given to it, as text.
struct PrepareRequest {
  std::string gas;
  std::string script;
  std::map<std::string, plan::PrepareValue> variables;
};
struct PrepareReport {
  TimePoint started{}, finished{};
  std::map<std::string, std::string> variables;
};
class IPrepareScriptPort {
 public:
  virtual ~IPrepareScriptPort() = default;
  // An error ends the measurement Failed, unless the token was cancelled.
  virtual Result<PrepareReport> run(const PrepareRequest& request, scripting::CancelToken& token) = 0;
};

// Measurement hook (spec 4.3): Python called at named points. A hook that
// does not define `entry` is a no-op.
class IMeasurementHook {
 public:
  virtual ~IMeasurementHook() = default;
  virtual Result<void> call(std::string_view entry, scripting::IMeasurementApi& api, scripting::CancelToken& token,
                            const scripting::ValueMap& args = {}) = 0;
};

}  // namespace pychron::experiment::measurement
