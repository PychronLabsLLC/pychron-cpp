#include "line_settings.hpp"

#include "pychron/core/config/loader.hpp"
#include "pychron/core/config/logging_file.hpp"
#include "pychron/core/config/local_file.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace pychron::ui {

namespace {

// `<root>/logging.local.toml` is there and does not load.
bool local_logging_broken(const std::filesystem::path& install_root) {
  const std::filesystem::path file = config::logging_local_file(install_root);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(file, ec)) return false;
  std::ifstream in(file, std::ios::binary);
  if (!in) return true;
  const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  config::LoggingConfig scratch;
  return !config::apply_logging_toml(text, file.generic_string(), scratch);
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
    // Everything beneath logging.local.toml: what a preference is compared
    // with and what Restore Defaults goes back to.
    const auto below = config::load_logging_config(install_root, main_file, false);
    s.install_root = install_root;
    if (sources && below) {
      s.logging = sources->config;
      s.shared_logging = below->config;
    } else {
      // As the program runs: on the built-in defaults.
      s.logging = config::LoggingConfig{};
      s.shared_logging = config::LoggingConfig{};
      s.logging_problem = QString::fromStdString((below ? sources.error() : below.error()).what);
      s.logging_writable = !local_logging_broken(*install_root);  // a broken local file is left alone
    }
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
  if (wanted.logging_writable) {
    const auto written = wanted.install_root
                             ? config::replace_local_logging(config::logging_local_file(*wanted.install_root), logging)
                             : config::replace_local_table(local, "logging", logging);
    if (!written) return QString::fromStdString(written.error().what);
  }
  if (const auto r = config::replace_local_table(local, "metrics", metrics); !r) {
    return QString::fromStdString(r.error().what);
  }
  return {};
}

}  // namespace pychron::ui
