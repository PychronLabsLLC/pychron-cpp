#include "line_settings.hpp"

#include "pychron/core/config/loader.hpp"
#include "pychron/core/config/logging_file.hpp"
#include "pychron/core/config/local_file.hpp"

namespace pychron::ui {

std::optional<LineSettings> load_line_settings(const std::filesystem::path& main_file,
                                               const std::optional<std::filesystem::path>& install_root) {
  const auto in_force = config::load_system_config(main_file);
  const auto shared = config::load_system_config_without_local(main_file);
  if (!in_force || !shared) return std::nullopt;
  LineSettings s;
  s.logging = in_force->logging;
  s.metrics = in_force->metrics;
  s.shared_logging = shared->logging;
  s.shared_metrics = shared->metrics;
  if (install_root) {
    const auto sources = config::load_logging_config(install_root, main_file);
    // Everything beneath logging.local.toml: what a preference is compared
    // with and what Restore Defaults goes back to.
    const auto below = config::load_logging_config(install_root, main_file, false);
    if (!sources || !below) return std::nullopt;
    s.logging = sources->config;
    s.shared_logging = below->config;
    s.install_root = install_root;
  }
  return s;
}

QString save_line_settings(const std::filesystem::path& main_file, const LineSettings& wanted) {
  const std::filesystem::path local = config::local_override_path(main_file);
  // Both texts are worked out before either is written, and the first write
  // is the only one that can find the file unusable.
  const std::string logging = wanted.install_root
                                  ? config::logging_local_toml(wanted.logging, wanted.shared_logging)
                                  : config::logging_override_toml(wanted.logging, wanted.shared_logging);
  const std::string metrics = config::metrics_override_toml(wanted.metrics, wanted.shared_metrics);
  const auto written = wanted.install_root
                           ? config::replace_local_logging(config::logging_local_file(*wanted.install_root), logging)
                           : config::replace_local_table(local, "logging", logging);
  if (!written) return QString::fromStdString(written.error().what);
  if (const auto r = config::replace_local_table(local, "metrics", metrics); !r) {
    return QString::fromStdString(r.error().what);
  }
  return {};
}

}  // namespace pychron::ui
