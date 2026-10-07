#pragma once

// Typed model of `extraction_line.toml` (spec section 5.1).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <toml++/toml.hpp>

#include "pychron/core/config/located.hpp"
#include "pychron/core/config/logging_config.hpp"

namespace pychron::config {

struct SystemSection : Located {
  std::string name;
  std::int64_t scan_interval_ms = 1000;
};

// `[sim]`: for a line with simulated transports.
struct SimSection : Located {
  // The simulator's numbers (sim.toml), relative to this file. Empty: a
  // `sim.toml` beside this file, if there is one.
  std::string file;
};

enum class TransportKind { Serial, Tcp, Udp, ModbusRtu, ModbusTcp, Sim, Link };

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

// A connected UDP socket to one peer (Qtegra over UDP).
struct UdpParams {
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

// A connection another config owns (NGX); see LinkTransport.
struct LinkParams {
  std::string link;
};

using TransportParams =
    std::variant<SerialParams, TcpParams, UdpParams, ModbusRtuParams, ModbusTcpParams, SimParams, LinkParams>;

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

// Where a valve's state is read when not from its own actuator (LDEO reads
// autovalve states from a second Agilent): `state_source = { driver =
// "agilent1", address = "102", inverted = false }`. `inverted`: that input
// reads true when the valve is closed.
struct StateSourceConfig : Located {
  std::string driver;  // name of a [drivers.*] entry
  std::string address;
  bool inverted = false;
};

// Actuated valves and switches share these keys:
//   inverted      the actuator's channel is wired backwards: open() closes the
//                 valve and its read-back says the opposite. The recorded
//                 state is always the valve's.
//   state_source  read the state somewhere else (see StateSourceConfig).
//   verify        false: no read-back; the commanded state is recorded.
//                 Legacy `query_state = false`. Not with state_source.
struct ValveConfig : Located {
  std::string name;
  std::string description;
  std::string actuator;  // name of a [drivers.*] entry
  std::string address;
  std::vector<std::string> interlocks;           // cannot open while any of these are open
  std::vector<std::string> positive_interlocks;  // all must be open before this opens
  std::int64_t settle_ms = 0;
  bool inverted = false;
  std::optional<StateSourceConfig> state_source;
  bool verify = true;
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
  bool inverted = false;  // as for valves
  std::optional<StateSourceConfig> state_source;
  bool verify = true;
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

// [cryo]: the line's cryostat (plan 2026-10-05, C4). `driver` is a
// [drivers.*] that controls temperature (a Lake Shore). Setpoint n of a
// named setpoint goes to output n, and output n waits on input n, as legacy
// paired them. Values in kelvin.
struct CryoConfig : Located {
  std::string driver;
  double tolerance_k = 1.0;  // "at setpoint" band for a blocking set_cryo
  double timeout_s = 600;    // a blocking set_cryo that has not arrived by then fails
  // Legacy cryotemps.yaml: "He_freeze" = [14.0, 0.0] for outputs 1 and 2.
  std::map<std::string, std::vector<double>> setpoints;
};

// [[heaters]] (plan 2026-10-05, E1): a heater the line scans and the
// operator switches. `driver` is a [drivers.*] that heats; `units` is only
// shown (the controller's program decides them, e.g. "C").
struct HeaterConfig : Located {
  std::string name;
  std::string driver;
  std::string description;
  std::string units;
};

struct PipetteConfig : Located {
  std::string name;
  std::string inner;
  std::string outer;
};

// [aliases]: lab-specific names that measurement plans reference as
// '@key' (experiment spec 4.1), e.g. `valves.inlet = "B"` or
// `extraction.eqtime = 20`. Nested tables flatten to dotted keys.
// Aliases under `valves.` must name a configured valve or manual valve.
using AliasValue = std::variant<bool, std::int64_t, double, std::string>;

struct AliasConfig : Located {
  std::string key;  // dotted, e.g. "valves.inlet"
  AliasValue value;
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
  std::optional<CryoConfig> cryo;
  std::vector<HeaterConfig> heaters;
  LoggingConfig logging;
  std::map<std::string, AliasConfig> aliases;  // by key
  SimSection sim;
};

}  // namespace pychron::config
