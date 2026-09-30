#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/config.hpp"

namespace pychron::spectrometer::cfg {

// Outcome of parsing `spectrometer.toml`: `config` is set only when
// `diagnostics` is empty.
struct ConfigLoadReport {
  std::optional<SpectrometerConfig> config;
  std::vector<config::Diagnostic> diagnostics;

  bool ok() const noexcept { return config.has_value(); }
};

// Parse + merge the optional `*.local.toml` override (only
// [transports.<name>] host/port/baud/timeout_ms) + schema check: required
// fields, types, enum values, ranges, no unknown sections or keys. Does NOT
// run the assembler rules (config_validate.hpp) because those need the field
// tables; load_spectrometer_data() runs both.
ConfigLoadReport parse_config_from_string(std::string_view main_toml, std::string_view main_name,
                                          std::optional<std::string_view> local_toml = std::nullopt,
                                          std::string_view local_name = "<local>");

// Reads `path` and, if present, its sibling `*.local.toml`.
ConfigLoadReport parse_config(const std::filesystem::path& path);

}  // namespace pychron::spectrometer::cfg
