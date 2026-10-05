#pragma once

// Events published on the SignalBus. All are plain values: state leaves the
// core only by copy, never by reference to a live object.

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "pychron/core/clock.hpp"
#include "pychron/core/error.hpp"

namespace pychron {

enum class ValveState { Unknown, Open, Closed };

enum class AlarmSeverity { Info, Warning, Critical };

enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Emitted by the Scheduler for every successful periodic scan.
struct Sample {
  std::string device;
  TimePoint ts{};
  double value = 0.0;
};

struct ValveChanged {
  std::string valve;
  ValveState state = ValveState::Unknown;
  TimePoint ts{};
};

struct PressureSample {
  std::string gauge;
  double value = 0.0;
  std::string units;
  TimePoint ts{};
};

// One reading of a temperature controller's input, in kelvin (the line's
// cryostat scan).
struct TemperatureSample {
  std::string source;  // the controller's driver name
  std::string input;   // "A", "B", ...
  double kelvin = 0.0;
  TimePoint ts{};
};

// One scan of a heater ([[heaters]]), in its configured units. A field the
// heater's driver does not support is nullopt.
struct HeaterSample {
  std::string heater;
  std::optional<double> readback;
  std::optional<double> setpoint;
  std::optional<bool> enabled;
  std::optional<bool> use_pid;
  TimePoint ts{};
};

struct Alarm {
  std::string source;
  AlarmSeverity severity = AlarmSeverity::Warning;
  std::string message;
  TimePoint ts{};
};

struct TransportHealth {
  std::string transport;
  bool connected = false;
  std::uint64_t error_count = 0;
  std::string last_error;
  TimePoint ts{};
};

struct Log {
  LogLevel level = LogLevel::Info;
  std::string logger;
  std::string message;
  TimePoint ts{};
};

// A valve or switch was software-locked or unlocked.
struct SwitchLockChanged {
  std::string name;
  bool locked = false;
  TimePoint ts{};
};

struct Snapshot {
  std::map<std::string, ValveState> valves;
  std::set<std::string> locked;  // software-locked valves and switches
  std::map<std::string, double> pressures;
  TimePoint ts{};
};

struct ActuationFailed {
  std::string valve;
  Error error;
  TimePoint ts{};
};

}  // namespace pychron
