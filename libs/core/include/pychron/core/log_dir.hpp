#pragma once

// The default log folder of an install, per operating system. Pure functions:
// the library never calls them; the apps do, once, at start-up.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "pychron/core/config/logging_config.hpp"

namespace pychron {

enum class HostOs { Mac, Linux, Windows };

// The OS this binary was built for.
[[nodiscard]] HostOs host_os() noexcept;

using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// Looks names up in the process environment.
[[nodiscard]] EnvLookup process_env();

// <install_root>/logs when there is an install root, else the OS folder:
// macOS ~/Library/Logs/pychron, Linux $XDG_STATE_HOME/pychron/logs
// (~/.local/state/pychron/logs when unset or empty), Windows
// %LOCALAPPDATA%\pychron\logs. Empty path when the variable that folder needs
// (HOME / LOCALAPPDATA) is unset or empty.
[[nodiscard]] std::filesystem::path default_log_dir(const std::optional<std::filesystem::path>& install_root,
                                                    HostOs os, const EnvLookup& env);

// The variable the OS folder cannot be found without: "LOCALAPPDATA" on
// Windows, else "HOME". For the note an app writes when default_log_dir is
// empty.
[[nodiscard]] const char* log_dir_env_name(HostOs os) noexcept;

// Sets config.dir to default_log_dir(...) when it is nullopt; leaves a given
// dir (empty included) alone.
void resolve_log_dir(config::LoggingConfig& config, const std::optional<std::filesystem::path>& install_root,
                     HostOs os, const EnvLookup& env);

}  // namespace pychron
