#pragma once

// elctl laser: trays and stage calibration for the lab's extraction devices
// (laser system design, section 6).
//
//   elctl [-c extraction_line.toml] [--sim] laser trays [--lab <dir>]
//   elctl ... laser calibrate <device> <tray> point <hole> [--x X --y Y]
//   elctl ... laser calibrate <device> <tray> center|right [--x X --y Y]
//   elctl ... laser calibrate <device> <tray> show|clear
//   elctl ... laser goto <device> <tray> <hole> [--timeout <seconds>]
//   elctl ... laser autocenter <device> <tray> <hole> [--timeout <seconds>]
//   elctl ... laser corrections <device> <tray> [clear [<hole>]]
//   elctl ... laser look <device> [--tray <tray>]
//   elctl ... laser patterns
//   elctl ... laser pattern <device> <name> [--dry-run] [--timeout <seconds>]
//   elctl laser track <scenario.toml> [--seeds N] [--trace <file>] [--frames <dir>]
//                     [--frames-seed S] [--check]
//
// A calibration is a list of points: "the stage was at (x, y) when it was on
// this hole". `point` takes the position from the device (jog the stage onto
// the hole first, with the laser's own software) unless --x and --y give it,
// in which case no hardware is opened. `center` and `right` are `point` at
// the tray map's center and east calibration holes. One point places the
// tray, two also turn it, three or more are fitted.
//
// `autocenter` needs the device to have a camera (<lab>/cameras.toml): it
// moves to the hole, centers it, and saves where it was found. `look` only
// says what the camera's finder sees. `corrections` lists or forgets what
// autocenter has saved.
//
// `pattern` runs a pattern about wherever the stage is, without firing the
// laser: to see where the beam would go. goto and pattern stop the stage on
// Ctrl-C and when --timeout runs out.
//
// `track` runs the dragonfly pattern against a simulated grain and scores it
// (grain simulator design, section 7). It builds a simulator of its own and
// needs no line config, lab or device; whatever -c names is left alone.
//
// --lab defaults to the install's lab, else the -c config's directory.

#include <string>
#include <vector>

#include "cli.hpp"
#include "exp.hpp"
#include "pychron/laser/track_bench.hpp"

namespace elctl {

// `words` is {"track", <scenario>}. Prints the table; a bad scenario is an
// error on stderr and kFailed.
int laser_track(const std::vector<std::string>& words, const pychron::laser::TrackOptions& options, Io io);

int laser_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);

}  // namespace elctl
