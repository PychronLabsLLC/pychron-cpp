#pragma once

// What File > Preferences shows on its Logging and Metrics pages: the line's
// `[logging]` and `[metrics]` as they are in force on this computer, and as
// the shared config alone has them.
//
// These are not QSettings like the other preferences. They belong to the
// line, and elctl must log and publish the same way the window does, so they
// are kept where the line reads them: in the local override file beside its
// config (extraction_line.local.toml), which holds only what differs from
// the shared file. An install's logging is the exception: it is kept per
// install, in `<root>/logging.local.toml` over `<root>/logging.toml`, which is
// what Restore Defaults goes back to (built-in defaults when there is none).

#include <filesystem>
#include <optional>

#include <QString>

#include "pychron/core/config/logging_config.hpp"
#include "pychron/core/config/metrics_config.hpp"

namespace pychron::ui {

struct LineSettings {
  config::LoggingConfig logging;         // in force here: the shared file with the local one over it
  config::MetricsConfig metrics;
  config::LoggingConfig shared_logging;  // the shared file alone: what Restore Defaults goes back to
  config::MetricsConfig shared_metrics;
  QString metrics_status;                // what the endpoint is doing now, for the Metrics page
  // The install the line belongs to: its logging is read from and written to
  // the install's logging files. Unset (examples, files given): the line's own
  // `[logging]`.
  std::optional<std::filesystem::path> install_root;
};

// From `main_file` and its local override (and `install_root`'s logging files).
// Nullopt when any cannot be loaded: the pages are then left out.
std::optional<LineSettings> load_line_settings(const std::filesystem::path& main_file,
                                               const std::optional<std::filesystem::path>& install_root = std::nullopt);

// Writes `wanted`'s metrics into `main_file`'s local override and its logging
// into that or, with an install root, `logging.local.toml`, as far as they
// differ from the shared file; what does not differ is taken
// out of it. Empty when it is done, else what to tell the user (the file is
// then as it was).
QString save_line_settings(const std::filesystem::path& main_file, const LineSettings& wanted);

}  // namespace pychron::ui
