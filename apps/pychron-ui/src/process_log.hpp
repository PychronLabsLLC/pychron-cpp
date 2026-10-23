#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include "pychron/core/clock.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::ui {

// The process log hub of pychron-ui, as elctl's: loads the logging config
// (install root, `line_config` as the legacy table), file_name "pychron.log",
// resolves the folder, creates the hub on `clock`, logs the config notes and
// begins the session. `clock_name` is what the session line says of the clock
// ("steady", "virtual x10"). Never fails: on error, one stderr line, null hub.
std::shared_ptr<LogHub> open_process_log(const std::optional<setup::SiteInstall>& install,
                                         const std::optional<std::filesystem::path>& line_config,
                                         const Clock& clock, bool simulated, HostOs os, const EnvLookup& env,
                                         std::string_view clock_name = "steady");

}  // namespace pychron::ui
