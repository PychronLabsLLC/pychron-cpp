#pragma once

// What File > Preferences shows on its Logging and Metrics pages: the line's
// `[logging]` and `[metrics]` as they are in force on this computer, and as
// the shared config alone has them.
//
// These are not QSettings like the other preferences. They belong to the
// line, and elctl must log and publish the same way the window does, so they
// are kept where the line reads them: in the local override file beside its
// config (extraction_line.local.toml), which holds only what differs from
// the shared file.

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
};

// From `main_file` and its local override. Nullopt when either cannot be
// loaded: the pages are then left out.
std::optional<LineSettings> load_line_settings(const std::filesystem::path& main_file);

// Writes `wanted`'s logging and metrics into `main_file`'s local override,
// as far as they differ from the shared file; what does not differ is taken
// out of it. Empty when it is done, else what to tell the user (the file is
// then as it was).
QString save_line_settings(const std::filesystem::path& main_file, const LineSettings& wanted);

}  // namespace pychron::ui
