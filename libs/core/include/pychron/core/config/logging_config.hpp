#pragma once

// Typed model of the optional `[logging]` table of `extraction_line.toml`.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "pychron/core/config/located.hpp"
#include "pychron/core/events.hpp"

namespace pychron::config {

struct LoggingConfig : Located {
  std::filesystem::path dir;  // empty = no file sink
  std::int64_t max_size_mb = 10;
  std::int64_t max_files = 5;
  LogLevel default_level = LogLevel::Info;
  std::vector<std::pair<std::string, LogLevel>> levels;  // logger-name glob -> level
  bool echo_stderr = false;
};

}  // namespace pychron::config
