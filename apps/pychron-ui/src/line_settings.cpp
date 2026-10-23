#include "line_settings.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "pychron/core/config/loader.hpp"
#include "pychron/core/config/logging_file.hpp"
#include "pychron/core/config/local_file.hpp"

namespace pychron::ui {

namespace {

// What <root>/logging.toml says over the built-in defaults: what Restore
// Defaults goes back to. Nullopt when the file is there and does not parse.
std::optional<config::LoggingConfig> shared_install_logging(const std::filesystem::path& root) {
  config::LoggingConfig out;
  const auto file = config::logging_file(root);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return out;
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  if (!config::apply_logging_toml(text.str(), file.generic_string(), out)) return std::nullopt;
  return out;
}

}  // namespace

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
    const auto shared_logging = shared_install_logging(*install_root);
    if (!sources || !shared_logging) return std::nullopt;
    s.logging = sources->config;
    s.shared_logging = *shared_logging;
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
