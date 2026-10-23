#include "pychron/core/config/logging_file.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "pychron/core/config/loader.hpp"

namespace pychron::config {

namespace {

bool is_file(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}

Result<std::string> slurp(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, p.generic_string() + ": cannot be read");
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

std::filesystem::path logging_file(const std::filesystem::path& install_root) { return install_root / "logging.toml"; }

std::filesystem::path logging_local_file(const std::filesystem::path& install_root) {
  return install_root / "logging.local.toml";
}

Result<void> apply_logging_toml(std::string_view toml, std::string_view name, LoggingConfig& into) {
  return parse_logging_text(toml, name, into);
}

Result<LoggingSources> load_logging_config(const std::optional<std::filesystem::path>& install_root,
                                           const std::optional<std::filesystem::path>& line_config,
                                           bool include_local_file) {
  LoggingSources out;
  std::optional<std::filesystem::path> legacy_path;
  if (line_config) {
    auto line = load_system_config(*line_config);
    if (!line) {
      out.notes.emplace_back(LogLevel::Warn, "the [logging] table of " + line_config->generic_string() +
                                                 " is not read: the file does not load (" + line.error().what + ")");
    } else {
      out.config = line->logging;
      // The files that gave the table, lowest precedence first.
      out.files.push_back(*line_config);
      const auto local = local_override_path(*line_config);
      if (is_file(local)) out.files.push_back(local);
      if (!line->logging.path.empty()) legacy_path = *line_config;
    }
  }

  bool overridden = false;
  if (install_root) {
    for (const auto& file : {logging_file(*install_root), logging_local_file(*install_root)}) {
      if (!include_local_file && file == logging_local_file(*install_root)) continue;
      if (!is_file(file)) continue;
      auto text = slurp(file);
      if (!text) return fail(text.error());
      if (auto applied = apply_logging_toml(*text, file.generic_string(), out.config); !applied) {
        return fail(applied.error());
      }
      out.files.push_back(file);
      overridden = true;
    }
  }

  if (legacy_path) {
    if (overridden) {
      out.notes.emplace_back(LogLevel::Warn, "the [logging] table of " + legacy_path->generic_string() +
                                                 " is overridden by logging.toml in the install folder");
    } else {
      out.notes.emplace_back(LogLevel::Info, "the [logging] table of " + legacy_path->generic_string() +
                                                 " is still read; move it to <install>/logging.toml");
    }
  }
  return out;
}

}  // namespace pychron::config
