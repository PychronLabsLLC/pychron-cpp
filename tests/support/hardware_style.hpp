// A simulated lab's line file as an installed instrument's would be: every
// `kind = "sim"` transport on the wire, to an address nothing answers at, so
// only sim serve's endpoints can (sim serve spec section 7).
#pragma once

#include <set>
#include <sstream>
#include <string>

namespace pychron::testing {

// TEST-NET-1 (RFC 5737): reserved for documentation, routed nowhere.
inline constexpr const char* kNowhere = "192.0.2.1";

// `toml` with each `[transports.<name>]`'s `kind = "sim"` made tcp to
// kNowhere, or modbus_tcp for the names in `modbus`, each on a port of its own.
[[nodiscard]] inline std::string hardware_style(const std::string& toml,
                                                const std::set<std::string>& modbus = {"bakeout_plc"}) {
  std::istringstream in(toml);
  std::ostringstream out;
  std::string line;
  std::string transport;  // the [transports.<name>] table the line is in, if any
  int port = 4001;
  while (std::getline(in, line)) {
    if (line.starts_with('[')) {
      const std::string prefix = "[transports.";
      transport = line.starts_with(prefix) && line.ends_with(']')
                      ? line.substr(prefix.size(), line.size() - prefix.size() - 1)
                      : std::string{};
    }
    if (!transport.empty() && line == "kind = \"sim\"") {
      out << "kind = \"" << (modbus.contains(transport) ? "modbus_tcp" : "tcp") << "\"\n"
          << "host = \"" << kNowhere << "\"\n"
          << "port = " << port++ << '\n';
      continue;
    }
    out << line << '\n';
  }
  return out.str();
}

}  // namespace pychron::testing
