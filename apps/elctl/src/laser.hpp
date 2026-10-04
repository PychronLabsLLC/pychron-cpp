#pragma once

// elctl laser: trays and stage calibration for the lab's extraction devices
// (laser system design, section 6).
//
//   elctl [-c extraction_line.toml] [--sim] laser trays [--lab <dir>]
//   elctl ... laser calibrate <device> <tray> point <hole> [--x X --y Y]
//   elctl ... laser calibrate <device> <tray> center|right [--x X --y Y]
//   elctl ... laser calibrate <device> <tray> show|clear
//   elctl ... laser goto <device> <tray> <hole> [--timeout <seconds>]
//
// A calibration is a list of points: "the stage was at (x, y) when it was on
// this hole". `point` takes the position from the device (jog the stage onto
// the hole first, with the laser's own software) unless --x and --y give it,
// in which case no hardware is opened. `center` and `right` are `point` at
// the tray map's centre and east calibration holes. One point places the
// tray, two also turn it, three or more are fitted.
//
// --lab defaults to the install's lab, else the -c config's directory.

#include <string>
#include <vector>

#include "cli.hpp"
#include "exp.hpp"

namespace elctl {

int laser_command(const std::vector<std::string>& args, const ExpGlobals& globals, Io io);

}  // namespace elctl
