#pragma once

// Where sim serve answers for each transport of a line (sim serve spec,
// docs/superpowers/specs/2026-10-09-sim-serve-design.md section 4.5). Written
// by `elctl sim serve`, read by a client given `--sim-endpoints <file>`, never
// edited by hand:
//
//   config = "/abs/path/extraction_line.toml"
//   host = "127.0.0.1"
//
//   [transports.switch_controller]
//   protocol = "tcp"             # or "udp"
//   port = 40213
//   emulator = "agilent_switch"  # "" = silent wire
//
// With a spectrometer served too (phase B), its transports by the
// spectrometer file's own names, which may be a line transport's:
//
//   spectrometer = "/abs/path/spectrometer.toml"
//
//   [spectrometer_transports.qtegra]
//   protocol = "tcp"
//   port = 40214
//   emulator = "thermo_qtegra"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "pychron/core/error.hpp"

namespace pychron {

enum class WireProtocol { Tcp, Udp };

struct Endpoint {
  std::string host;
  std::uint16_t port = 0;
  WireProtocol protocol = WireProtocol::Tcp;
  std::string emulator;  // the driver kind(s) answered as; "" = silent wire

  friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

struct SimEndpoints {
  std::filesystem::path config;                // the served line's file, absolute
  std::string host;                            // what clients connect to
  std::map<std::string, Endpoint> transports;  // by transport name; each entry's host is `host`
  std::filesystem::path spectrometer;          // the served spectrometer's file; empty: none
  std::map<std::string, Endpoint> spectrometer_transports;  // by the spectrometer file's names

  friend bool operator==(const SimEndpoints&, const SimEndpoints&) = default;
};

// Config error, naming the file, when it cannot be read or says something
// this format does not (a protocol other than tcp or udp, a port outside
// 1..65535).
Result<SimEndpoints> read_sim_endpoints(const std::filesystem::path& file);
// Written beside the file and moved over it: a client never reads half of one.
Result<void> write_sim_endpoints(const std::filesystem::path& file, const SimEndpoints& endpoints);
// read_sim_endpoints, then a Config error when the file's `config` is not
// `loaded_config` (compared as canonical paths), unless `any`.
Result<SimEndpoints> load_sim_endpoints(const std::filesystem::path& file,
                                        const std::filesystem::path& loaded_config, bool any);
// The spectrometer section of `file`: Config error when the file serves no
// spectrometer, or one other than `loaded_spectrometer` (canonical paths),
// unless `any`.
Result<std::map<std::string, Endpoint>> load_spectrometer_endpoints(const std::filesystem::path& file,
                                                                    const std::filesystem::path& loaded_spectrometer,
                                                                    bool any);
// sim-endpoints.toml beside the line's file.
std::filesystem::path default_sim_endpoints_path(const std::filesystem::path& line_file);

}  // namespace pychron
