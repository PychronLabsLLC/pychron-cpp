#pragma once

// The hardware surface the MeasurementEngine uses (experiment spec 4.2):
// spectrometer primitives, extraction-line valves, the peak-center job and the
// measurement hook. Narrow interfaces so the engine runs against fakes in unit
// tests and against the real facades through adapters.hpp.

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
  // Puts target.isotope (or target.mass) on target.detector. No settle: the
  // engine settles itself so the wait is cancellable.
  virtual Result<void> position(const plan::HopTarget& target) = 0;
  virtual Result<void> protect(const std::string& detector, bool on) = 0;
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
};

struct PeakCenterReport {
  PeakCenterRequest request;
  bool ok = false;
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
