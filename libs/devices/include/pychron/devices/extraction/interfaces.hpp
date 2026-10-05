#pragma once

// Typed extraction-device interfaces the script host binds its vocabulary to
// (spec section 6). IExtractionDevice is required for every extraction run;
// the rest are optional features reached through its accessors (nullptr when
// absent) or, for line services, through ExtractionServices.
//
//   extract end_extract enable disable prepare       IExtractionDevice
//   fire_laser warmup                                ILaserDevice
//   set_pid_parameters dump_sample drop_sample       IFurnaceDevice
//   move_to_position set_x set_y set_z set_xy
//   set_tray                                         IStage
//   execute_pattern                                  IPatternRunner
//   load_pipette extract_pipette                     IPipetteService
//   set_cryo get_cryo_temp                           ICryo
//   set_motor get_value                              IMotorService
//   snapshot video_*                                 IImaging
//   get_pressure get_manometer_pressure              IPressureService
//   open close lock unlock is_open is_closed         IValveService
//
// Calls block until the hardware answers (or its transport times out) and
// are made from the script host's thread, never the UI thread. Waiting on
// long motions (moves, patterns) is the caller's job: start, then poll the
// matching *_moving()/running() query while checking its CancelToken.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/devices/extraction/capability.hpp"

namespace pychron::extraction {

// `extract(value, units)`: pychron's 'percent', 'watts' and 'temp'.
enum class ExtractUnits { Percent, Watts, Celsius };

std::string_view to_string(ExtractUnits units) noexcept;
std::optional<ExtractUnits> extract_units_from_string(std::string_view name) noexcept;

struct ILaserDevice {
  virtual ~ILaserDevice() = default;
  // Open the beam at the current extract() output.
  virtual Result<void> fire_laser() = 0;
  // Close the beam; the output setpoint is kept.
  virtual Result<void> stop_laser() = 0;
  virtual Result<bool> is_firing() = 0;
  // Pre-extraction warmup (e.g. CO2 tube conditioning).
  virtual Result<void> warmup() = 0;
  // The interlocks tripped now, by name; empty when every one is satisfied,
  // and for a device that cannot say.
  virtual Result<std::vector<std::string>> tripped_interlocks() { return std::vector<std::string>{}; }
};

struct IFurnaceDevice {
  virtual ~IFurnaceDevice() = default;
  virtual Result<double> read_temperature() = 0;  // Celsius
  // Select the PID parameter set tuned for output `value`.
  virtual Result<void> set_pid_parameters(double value) = 0;
  // Drop the sample at `position` into the crucible.
  virtual Result<void> drop_sample(std::string_view position) = 0;
  // Empty the crucible.
  virtual Result<void> dump_sample() = 0;
};

struct StagePosition {
  double x = 0;
  double y = 0;
  double z = 0;
  friend bool operator==(const StagePosition&, const StagePosition&) = default;
};

struct IStage {
  enum class Axis { X, Y, Z };

  virtual ~IStage() = default;
  // Named hole/position on the current tray. Config error for an unknown
  // name. Starts the move; poll moving().
  virtual Result<void> move_to_position(std::string_view position, bool autocenter) = 0;
  virtual Result<void> set_axis(Axis axis, double value) = 0;
  // `speed_mm_s` 0: the stage's own travel speed. A stage that cannot set a
  // speed moves at its own; a negative or non-finite speed is a Config error.
  // (A default argument is bound to the static type: overrides repeat it.)
  virtual Result<void> set_xy(double x, double y, double speed_mm_s = 0) = 0;
  virtual Result<StagePosition> position() = 0;
  virtual Result<bool> moving() = 0;
  // Stop any motion now, where it is; moving() is false after. A stage that
  // cannot be stopped answers not_supported().
  virtual Result<void> stop() { return fail(not_supported("stage stop")); }
  // True for a stage whose move_to_position(..., autocenter = true) goes on
  // working after the stage arrives, and only while moving() is asked: a
  // move nobody waits for would be left uncentred, its failure unseen.
  virtual bool autocenter_needs_polling() const { return false; }
  // What the last finished hole move has to say for the run's log (centred
  // and by how much; not centred and why); empty when nothing. Said once.
  virtual std::string last_move_note() { return {}; }
  // Config error for an unknown tray.
  virtual Result<void> set_tray(std::string_view tray) = 0;
  virtual std::vector<std::string> positions() const = 0;  // on the current tray
};

struct IPatternRunner {
  virtual ~IPatternRunner() = default;
  // Config error for an unknown pattern. Starts it; poll running().
  virtual Result<void> execute_pattern(std::string_view pattern) = 0;
  // The same, told how long the run heats for (seconds; 0 or less: it does
  // not say). A pattern that runs for a time (one that follows the glow)
  // runs for that; one with a path of its own ignores it.
  virtual Result<void> execute_pattern_for(std::string_view pattern, double duration_s) {
    (void)duration_s;
    return execute_pattern(pattern);
  }
  virtual Result<bool> running() = 0;
  virtual Result<void> stop_pattern() = 0;
  virtual std::vector<std::string> patterns() const = 0;
  // True for a runner that advances only when running() is asked: a pattern
  // nobody waits for would stay on its first point. False for a device that
  // runs its patterns itself.
  virtual bool needs_polling() const { return false; }
  // What the last finished pattern has to say for the run's log; empty when
  // nothing. Said once.
  virtual std::string last_note() { return {}; }
};

struct IPipetteService {
  virtual ~IPipetteService() = default;
  // Fill pipette `name` (inner then outer valve sequence).
  virtual Result<void> load_pipette(std::string_view name) = 0;
  // Release its aliquot into the line.
  virtual Result<void> extract_pipette(std::string_view name) = 0;
  virtual std::vector<std::string> pipette_names() const = 0;
};

struct ICryo {
  virtual ~ICryo() = default;
  virtual Result<void> set_cryo(double setpoint_kelvin) = 0;
  virtual Result<double> get_cryo_temp(int channel) = 0;  // Kelvin
};

struct IMotorService {
  virtual ~IMotorService() = default;
  // Config error for an unknown motor. Starts the move.
  virtual Result<void> set_motor(std::string_view name, double value) = 0;
  virtual Result<double> get_value(std::string_view name) = 0;
  virtual Result<bool> motor_moving(std::string_view name) = 0;
  virtual std::vector<std::string> motor_names() const = 0;
};

struct IImaging {
  virtual ~IImaging() = default;
  // Saves a frame; returns where (host-relative path or identifier).
  virtual Result<std::string> snapshot(std::string_view name) = 0;
  virtual Result<void> start_video_recording(std::string_view name) = 0;
  virtual Result<void> stop_video_recording() = 0;
};

// The extraction device of a run: laser, furnace, or any heater that turns
// an output setpoint into gas release.
struct IExtractionDevice {
  virtual ~IExtractionDevice() = default;

  virtual const std::string& device_name() const = 0;

  // Called once before the first extract() of a run.
  virtual Result<void> prepare() { return {}; }
  virtual Result<void> enable() = 0;
  // Disabling also ends any extraction (output 0).
  virtual Result<void> disable() = 0;
  virtual Result<bool> is_enabled() = 0;

  // Set the output. Negative values and unsupported units are Config errors;
  // extracting while disabled is an Interlock error. Nothing is changed on
  // error.
  virtual Result<void> extract(double value, ExtractUnits units) = 0;
  // Output to 0 (and stop firing, for lasers). Safe to call at any time.
  virtual Result<void> end_extract() = 0;
  // The last accepted extract() setpoint in its units; 0 after end_extract().
  virtual Result<double> output() = 0;
  virtual bool supports(ExtractUnits units) const = 0;

  // Optional features. The same object for the device's lifetime.
  virtual ILaserDevice* laser() { return nullptr; }
  virtual IFurnaceDevice* furnace() { return nullptr; }
  virtual IStage* stage() { return nullptr; }
  virtual IPatternRunner* pattern_runner() { return nullptr; }
  virtual IPipetteService* pipettes() { return nullptr; }
  virtual ICryo* cryo() { return nullptr; }
  virtual IMotorService* motors() { return nullptr; }
  virtual IImaging* imaging() { return nullptr; }
};

// Features `device` exposes through its accessors.
CapabilitySet capabilities(IExtractionDevice& device);

}  // namespace pychron::extraction
