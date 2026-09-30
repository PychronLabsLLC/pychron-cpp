#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"

namespace pychron::test {

// The example from spec section 5.1, verbatim.
inline constexpr std::string_view kExampleConfig = R"toml(
[system]
name = "jan"
scan_interval_ms = 1000

[transports.valve_bus]
kind = "serial"            # serial | tcp | modbus_rtu | modbus_tcp | sim
port = "/dev/tty.usbserial-A1"   # "COM4" on Windows
baud = 9600
timeout_ms = 500
retries = 2
trace = true

[transports.gauge_net]
kind = "tcp"
host = "192.168.0.51"
port = 8000

[drivers.actuator1]
kind = "proxr_relay"
transport = "valve_bus"

[drivers.ig_controller]
kind = "pfeiffer_maxigauge"
transport = "gauge_net"
channels = [1, 2, 3]

[[valves]]
name = "A"
description = "Furnace to bone"
actuator = "actuator1"
address = "1"
interlocks = ["B"]           # cannot open while any of these are open
positive_interlocks = []     # all of these must be open before this opens
settle_ms = 1000

[[valves]]
name = "B"
actuator = "actuator1"
address = "2"

[[manual_valves]]
name = "M1"

[[switches]]
name = "pump_power"
description = "Turbo pump controller enable"
actuator = "actuator1"
address = "9"

[[gauges]]
name = "IG1"
driver = "ig_controller"
channel = 1
units = "torr"
alarm_high = 1e-4

[[pipettes]]
name = "air"
inner = "P1"
outer = "P2"
)toml";

// 1-based line number of the first line containing `needle`.
inline std::uint32_t line_of(std::string_view text, std::string_view needle) {
  const auto pos = text.find(needle);
  return static_cast<std::uint32_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n') + 1);
}

// Minimal valid preamble (11 lines); tests append [[valves]] etc.
inline constexpr std::string_view kPreamble = R"toml([system]
name = "t"
[transports.bus]
kind = "sim"
[drivers.act]
kind = "proxr_relay"
transport = "bus"
[drivers.gc]
kind = "pfeiffer_maxigauge"
transport = "bus"
channels = [1, 2]
)toml";

inline std::vector<std::string> formatted(const std::vector<config::Diagnostic>& ds) {
  std::vector<std::string> out;
  for (const auto& d : ds) out.push_back(config::to_string(d));
  return out;
}

// True if some diagnostic formats exactly as `line`.
inline bool has(const std::vector<config::Diagnostic>& ds, std::string_view line) {
  auto f = formatted(ds);
  return std::find(f.begin(), f.end(), line) != f.end();
}

}  // namespace pychron::test
