#pragma once

// Typed model of `extraction_line.toml` (spec section 5.1).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

namespace pychron::config {

// Where a value came from, for `file:line:field` diagnostics.
struct SourceLoc {
  std::string file;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

// Every configured entity records its own location, its dotted field path
// (e.g. "valves[2]" or "transports.valve_bus") and the location of each key it
// read, so later validation can point at the exact offending line.
struct Located {
  SourceLoc loc;
  std::string path;
  std::map<std::string, SourceLoc> field_locs;

  // Location of `field` (e.g. "actuator", "interlocks[1]"), or the entity's own.
  const SourceLoc& where(const std::string& field) const {
    auto it = field_locs.find(field);
    return it == field_locs.end() ? loc : it->second;
  }
};

struct SystemSection : Located {
  std::string name;
  std::int64_t scan_interval_ms = 1000;
};

enum class TransportKind { Serial, Tcp, ModbusRtu, ModbusTcp, Sim };

enum class Parity { None, Even, Odd };

struct SerialParams {
  std::string port;  // "/dev/tty.usbserial-A1" or "COM4"
  std::int64_t baud = 9600;
  std::int64_t data_bits = 8;
  std::int64_t stop_bits = 1;
  Parity parity = Parity::None;
};

struct TcpParams {
  std::string host;
  std::int64_t port = 0;
};

struct ModbusRtuParams {
  SerialParams serial;
};

struct ModbusTcpParams {
  TcpParams tcp;  // port defaults to 502
};

struct SimParams {};

using TransportParams = std::variant<SerialParams, TcpParams, ModbusRtuParams, ModbusTcpParams, SimParams>;

struct TransportConfig : Located {
  std::string name;
  TransportKind kind = TransportKind::Sim;
  TransportParams params = SimParams{};
  std::int64_t timeout_ms = 500;
  std::int64_t retries = 0;
  bool trace = false;
};

struct DriverConfig : Located {
  std::string name;
  std::string kind;       // open set; resolved later by the DriverRegistry
  std::string transport;  // name of a [transports.*] entry
  std::vector<std::int64_t> channels;
  toml::table options;  // the whole [drivers.<name>] table, for driver-specific keys
};

struct ValveConfig : Located {
  std::string name;
  std::string description;
  std::string actuator;  // name of a [drivers.*] entry
  std::string address;
  std::vector<std::string> interlocks;           // cannot open while any of these are open
  std::vector<std::string> positive_interlocks;  // all must be open before this opens
  std::int64_t settle_ms = 0;
};

struct ManualValveConfig : Located {
  std::string name;
  std::string description;
};

// An actuated on/off thing that is not a gas valve (pump power, heater relay,
// shutter). Shares actuators and the address space with valves; never carries
// interlocks.
struct SwitchConfig : Located {
  std::string name;
  std::string description;
  std::string actuator;  // name of a [drivers.*] entry
  std::string address;
  std::int64_t settle_ms = 0;
};

enum class PressureUnits { Torr, Mbar, Pa };

struct GaugeConfig : Located {
  std::string name;
  std::string driver;
  std::int64_t channel = 1;
  PressureUnits units = PressureUnits::Torr;
  std::optional<double> alarm_high;
  std::optional<double> alarm_low;
};

struct PipetteConfig : Located {
  std::string name;
  std::string inner;
  std::string outer;
};

struct SystemConfig {
  std::string source_file;
  SystemSection system;
  std::map<std::string, TransportConfig> transports;
  std::map<std::string, DriverConfig> drivers;
  std::vector<ValveConfig> valves;
  std::vector<ManualValveConfig> manual_valves;
  std::vector<SwitchConfig> switches;
  std::vector<GaugeConfig> gauges;
  std::vector<PipetteConfig> pipettes;
};

}  // namespace pychron::config
