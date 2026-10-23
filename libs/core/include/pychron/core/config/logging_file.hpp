#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/core/logger.hpp"

namespace pychron::config {

// Applies the top-level keys of a logging.toml text over `into`. Diagnostics
// name `name`; an unknown key is an error, as in [logging]. On an error `into`
// is left as it was.
Result<void> apply_logging_toml(std::string_view toml, std::string_view name, LoggingConfig& into);

struct LoggingSources {
  LoggingConfig config;
  std::vector<std::filesystem::path> files;  // read, lowest precedence first
  // One line each, for the hub to log once it exists:
  // legacy overridden -> warn, legacy alone -> info.
  std::vector<std::pair<LogLevel, std::string>> notes;
};

// install_root: <root>/logging.toml and <root>/logging.local.toml.
// line_config: extraction_line.toml whose [logging] (and local) is the legacy
// table. A line config that does not load is not an error here: its table is
// skipped and a warn note says why. A logging.toml that does not parse is.
//
// include_local_file false leaves out <root>/logging.local.toml: everything
// beneath it, which is what File > Preferences compares with and what Restore
// Defaults goes back to.
Result<LoggingSources> load_logging_config(const std::optional<std::filesystem::path>& install_root,
                                           const std::optional<std::filesystem::path>& line_config,
                                           bool include_local_file = true);

std::filesystem::path logging_file(const std::filesystem::path& install_root);        // <root>/logging.toml
std::filesystem::path logging_local_file(const std::filesystem::path& install_root);  // <root>/logging.local.toml

}  // namespace pychron::config
