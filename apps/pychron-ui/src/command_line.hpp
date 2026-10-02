#pragma once

// pychron-ui's arguments (everything after the program name):
//
//   [extraction_line.toml [canvas.toml]] [--sim] [--spectrometer <file>]

#include <filesystem>
#include <optional>
#include <vector>

#include <QStringList>

#include "pychron/core/error.hpp"

namespace pychron::ui {

struct CommandLine {
  bool sim = false;
  std::vector<std::filesystem::path> files;  // positional, in order
  std::optional<std::filesystem::path> spectrometer_file;
};

// A Config error is a usage error: --spectrometer without a value, which
// includes one followed by another option ("--spectrometer --sim").
Result<CommandLine> parse_command_line(const QStringList& args);

}  // namespace pychron::ui
