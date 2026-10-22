#pragma once

// Typed model of the optional `[logging]` table of `extraction_line.toml`.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/config/located.hpp"
#include "pychron/core/events.hpp"

namespace pychron::config {

struct LoggingConfig : Located {
  std::optional<std::filesystem::path> dir;  // nullopt = the app's default folder; empty = no file
  std::string file_name = "pychron.log";     // set by the app, not a TOML key
  std::int64_t max_size_mb = 50;
  std::int64_t max_files = 10;
  LogLevel default_level = LogLevel::Debug;
  LogLevel dock_level = LogLevel::Info;      // what the log dock shows
  std::vector<std::pair<std::string, LogLevel>> levels;  // logger-name glob -> level
  bool echo_stderr = false;
};

}  // namespace pychron::config
