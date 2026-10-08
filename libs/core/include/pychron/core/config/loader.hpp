#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"

namespace pychron::config {

// Full outcome of a load: `config` is set only when `diagnostics` is empty.
struct LoadReport {
  std::optional<SystemConfig> config;
  std::vector<Diagnostic> diagnostics;

  bool ok() const noexcept { return config.has_value(); }
};

// Keys an `*.local.toml` override may set on an existing [transports.<name>].
bool is_overridable_transport_key(std::string_view key) noexcept;

// Whether `text` is written as an IPv4 or IPv6 address: what `[metrics] bind`
// must be. The spelling only; whether the address is this machine's is found
// out when something binds it.
bool is_ip_address(std::string_view text) noexcept;

// `extraction_line.toml` -> `extraction_line.local.toml` (same directory).
std::filesystem::path local_override_path(const std::filesystem::path& main_file);

// Parse + merge override + schema-check + cross-validate, collecting every
// problem. Loading is all-or-nothing.
LoadReport load_report_from_string(std::string_view main_toml, std::string_view main_name,
                                   std::optional<std::string_view> local_toml = std::nullopt,
                                   std::string_view local_name = "<local>");

// Reads `path` and, if it exists, its sibling `*.local.toml` override.
LoadReport load_report(const std::filesystem::path& path);

// Result form; the Error (kind Config) lists every diagnostic, one per line.
Result<SystemConfig> load_system_config(const std::filesystem::path& path);
Result<SystemConfig> load_system_config_from_string(std::string_view main_toml, std::string_view main_name,
                                                    std::optional<std::string_view> local_toml = std::nullopt,
                                                    std::string_view local_name = "<local>");

}  // namespace pychron::config
