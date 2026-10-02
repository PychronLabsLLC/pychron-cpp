#pragma once

// pychron-ui's arguments (everything after the program name):
//
//   [extraction_line.toml [canvas.toml]] [--sim] [--spectrometer <file>]
//   [--lab <dir>] [--data <dir>] [--queue <file>] [--sim-speed <x>] [--db <url>]

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <QStringList>

#include "pychron/core/error.hpp"

namespace pychron::ui {

struct CommandLine {
  bool sim = false;
  std::vector<std::filesystem::path> files;  // positional, in order
  std::optional<std::filesystem::path> spectrometer_file;
  std::optional<std::filesystem::path> lab;    // default: the line config's directory
  std::optional<std::filesystem::path> data;   // default: <lab>/data
  std::optional<std::filesystem::path> queue;  // opened in the experiment window
  double sim_speed = 0;                        // 0: real time
  std::optional<std::string> db;  // DVC store url: Window > Data browses it instead of the records
};

// A Config error is a usage error: an option without a value (which includes
// one followed by another option, "--spectrometer --sim"), a --sim-speed that
// is not a positive number, or --sim-speed without --sim.
Result<CommandLine> parse_command_line(const QStringList& args);

}  // namespace pychron::ui
