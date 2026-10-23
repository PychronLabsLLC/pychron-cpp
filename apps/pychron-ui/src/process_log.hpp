#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pychron/core/clock.hpp"
#include "pychron/core/log_dir.hpp"
#include "pychron/core/log_hub.hpp"
#include "pychron/setup/site.hpp"

namespace pychron::ui {

// The process log hub of pychron-ui, as elctl's: loads the logging config
// (install root, `line_config` as the legacy table), file_name "pychron.log",
// resolves the folder, creates the hub on `clock`, logs the config notes and
// begins the session. `clock_name` is what the session line says of the clock
// ("steady", "virtual x10").
//
// A logging config that does not load leaves the built-in defaults (and the
// default folder) in force; the reason is the first error record (logger
// `logging`), a stderr line and a notice. No folder (HOME / LOCALAPPDATA
// unset, `dir` not given) is a warn record, a stderr line and a notice. Null
// hub, with one stderr line, only when the back end itself cannot start.
struct ProcessLog {
  std::shared_ptr<LogHub> hub;
  // What the Log dock says once at start-up (the hub has no bus yet).
  std::vector<std::pair<LogLevel, std::string>> notices;
};
ProcessLog open_process_log(const std::optional<setup::SiteInstall>& install,
                                         const std::optional<std::filesystem::path>& line_config,
                                         const Clock& clock, bool simulated, HostOs os, const EnvLookup& env,
                                         std::string_view clock_name = "steady");

}  // namespace pychron::ui
