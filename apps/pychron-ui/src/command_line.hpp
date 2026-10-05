#pragma once

// pychron-ui's arguments (everything after the program name):
//
//   [extraction_line.toml [canvas.toml] | --install <name> | --setup | --examples]
//   [--sim] [--spectrometer <file>] [--lab <dir>] [--data <dir>] [--queue <file>]
//   [--sim-speed <x>] [--db <url>]
//   [--laser [--device <name>]]
//   --version | --self-test | --write-icons <dir>

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
  std::optional<std::string> db;  // DVC store url: View > Data browses it instead of the records
  std::optional<std::string> install;  // an install from the site config
  bool setup = false;                  // run the setup wizard first
  bool examples = false;               // the shipped example configs (development)
  bool laser = false;                  // only the laser window, for a laser PC
  std::optional<std::string> device;   // --laser: which extraction device (needed when the line has several)
  bool version = false;                // print the version and exit
  bool self_test = false;              // check an installed copy and exit
  std::optional<std::filesystem::path> write_icons;  // render the icon PNGs there and exit
};

// A Config error is a usage error: an unknown option, an option without a
// value (which includes one followed by another option, "--spectrometer
// --sim"), a --sim-speed that is not a positive number, --sim-speed without
// --sim, more than one of config files, --install, --setup, --examples,
// --device without --laser, or --laser with --queue or --spectrometer.
Result<CommandLine> parse_command_line(const QStringList& args);

}  // namespace pychron::ui
