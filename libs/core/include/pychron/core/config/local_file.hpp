#pragma once

// Writing the part of a `*.local.toml` the application owns: this computer's
// `[logging]` and `[metrics]`, set in File > Preferences. The file is also
// written by hand (a serial port, an address), so one table is replaced and
// everything else in it is left exactly as it was, comments included.

#include <filesystem>
#include <string>
#include <string_view>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/config/metrics_config.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"

namespace pychron::config {

// The TOML that, in the local file, turns `main` (what the shared file says)
// into `wanted`: only the keys that differ, and nothing at all ("") when none
// does. Levels are a whole list (the loader takes a local levels table as
// the list, not as additions), written when they differ in any way.
std::string logging_override_toml(const LoggingConfig& wanted, const LoggingConfig& main);
std::string metrics_override_toml(const MetricsConfig& wanted, const MetricsConfig& main);

// Replaces `[table]` and every `[table.*]` in `local_file` with `toml` (the
// tables' text, headers included; "" to remove them). The file is made when
// it is not there (readable by its owner only, as the setup wizard makes
// them), keeps its permissions when it is, and is removed when nothing is
// left in it.
//
// All or nothing: the result is checked to be TOML and written beside the
// file, then moved over it. A file that is not TOML to begin with, or a
// folder that cannot be written, is an error naming the file, which is left
// as it was.
Result<void> replace_local_table(const std::filesystem::path& local_file, std::string_view table,
                                 std::string_view toml);

// `main_file` without its local override: what the shared config says, which
// is what a preference is compared with and what Restore Defaults goes back to.
Result<SystemConfig> load_system_config_without_local(const std::filesystem::path& main_file);

}  // namespace pychron::config
