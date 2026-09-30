#pragma once

// Persistent `elctl trace on|off` state for one config file.
//
// The committed extraction_line.toml stays untouched and its `*.local.toml`
// may only carry port-class keys, so bring-up tracing is recorded in a
// sibling state file instead:
//   <config dir>/.elctl-trace     one transport name per line, "*" for all
// While it lists a transport, every elctl run records that transport's
// traffic to <config dir>/traces/<transport>.trace, the format
// SimTransport::replay() reads.

#include <filesystem>
#include <set>
#include <string>

#include "pychron/core/error.hpp"

namespace elctl {

struct TraceSettings {
  bool all = false;
  std::set<std::string> transports;

  bool enabled() const noexcept { return all || !transports.empty(); }
  bool traces(const std::string& transport) const { return all || transports.contains(transport); }
};

std::filesystem::path trace_state_path(const std::filesystem::path& config_file);
std::filesystem::path trace_dir(const std::filesystem::path& config_file);

// Missing state file = tracing off.
TraceSettings load_trace_settings(const std::filesystem::path& config_file);
// Removes the state file when `settings` is off. Io error if it cannot write.
pychron::Result<void> save_trace_settings(const std::filesystem::path& config_file, const TraceSettings& settings);

}  // namespace elctl
