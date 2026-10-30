#pragma once

// pychron-ui's arguments (everything after the program name):
//
//   [extraction_line.toml [canvas.toml] | --install <name> | --setup | --examples]
//   [--sim] [--spectrometer <file>] [--lab <dir>] [--data <dir>] [--queue <file>]
//   [--sim-speed <x>] [--db <url>] [--sim-save]
//   [--sim-endpoints <file> [--sim-endpoints-any]]
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
  double sim_speed = 0;                        // 0: real time; else simulated, this many times faster
  std::optional<std::string> db;  // DVC store url: View > Data browses it instead of the records
  bool sim_save = false;
  // A line served by `elctl sim serve`: its endpoints file. Refused with --sim
  // and --sim-speed (serve is real time).
  std::optional<std::filesystem::path> sim_endpoints;
  bool sim_endpoints_any = false;  // take a file serving another copy of the line  // a simulated session may save its runs to the database (database_choice.hpp)
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
// --sim"), a --sim-speed that is not a positive number (elctl's "max",
// unlimited speed, is refused by name), more than one of config files,
// --install, --setup, --examples, --device without --laser, or --laser with
// --queue or --spectrometer. --sim-speed without --sim is not refused here: an
// install can be one set up for simulation, which main() knows and this does not.
// --sim-endpoints with --sim or --sim-speed is refused here; with an install set
// up for simulation, by main().
Result<CommandLine> parse_command_line(const QStringList& args);

}  // namespace pychron::ui
