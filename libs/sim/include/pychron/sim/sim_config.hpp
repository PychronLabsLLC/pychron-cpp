#pragma once

// sim.toml: the numbers of a simulated lab (lab simulator spec section 6.2).
//
// Optional. A simulated line works with the defaults of `SimSettings`; this
// file overrides them, for the whole line or by name. It is read over a
// `base`: a key that is in the file replaces what the base has, and a key
// that is not leaves it. So what is in force is the built-in defaults, then
// what the caller set in the base, then the keys present in the file.
//
//   [defaults]             every volume and valve not named below
//     pressure             mbar, as air                    default_pressure
//     volume_cc            cc, a volume with no size       default_volume_cc
//     pipe_cc              cc, the pipe between two valves pipe_cc
//     gauge_cc             cc, a gauge with no size        gauge_cc
//     valve_conductance    L/s for Ar40                    valve_conductance
//     outgassing           mbar L / s of Ar40 per litre    outgassing
//     noise                relative 1-sigma on gauges      noise
//     seed                 of that noise, and of the       seed
//                          detectors' (feed_beam_from_line)
//   [compositions.<name>]  ratios to Ar36, by species      named
//     Ar36 Ar37 Ar38 Ar39 Ar40 active   (one not given is 0)
//   [volumes.<name>]       a stage, pipette or gauge; or "<a>~<b>", the pipe
//                          between valves a and b drawn joined (a before b
//                          in byte order, as std::string compares)
//     composition          `air`, `cocktail` or a [compositions.*] name
//     argon40              mbar of Ar40; the rest follows the composition
//     pressure             mbar in all; the composition's proportions
//     leak                 mbar L / s of Ar40, as air      leaks
//     volume_cc            cc                              sizes
//   [valves.<name>]
//     conductance          L/s for Ar40                    conductances
//   [pumps.<name>]         a pump on that volume           pumps, pump_speeds
//     speed                L/s
//     base                 mbar
//                          One not given is what the base's pump on that
//                          volume has; with no pump there, a pump stage's
//                          (pump_speed, pump_base).
//   [spectrometer]                                         source
//     sensitivity          fA per mbar of an isotope in the source
//     consumption          1/s
//     memory_fA_per_s      as Ar40
//   [detectors.<name>]                                     detectors
//     baseline             fA or cps
//     baseline_drift_per_h the same, per hour
//
// What a volume holds: `argon40` gives the composition (air if none is
// named) with that much Ar40; `pressure` gives it at that total; one or the
// other, not both. A composition named with neither is at a tank's
// `tank_argon40` on a tank and at the default pressure elsewhere. These land
// in `compositions`, or in `initial_pressures` for a pressure of air.
//
// Everything is checked, and every problem is reported, as
// `file:line:key: message` (a `Config` error). After a name or a key that is
// not known the message lists the ones that are (`; known: a, b, c`), or
// counts them when there are more than twelve; a number out of range is
// told its range with the unit.
//   - a key or a table the file may not have, a species that is not one;
//   - a volume, valve or pump name that `topology` does not have. The line
//     passes its canvas, and with it the gauges it has off the canvas (each
//     is a volume of its own once its controller is simulated), so a pump or
//     a volume may be named after one of those. Detector names are the
//     spectrometer's and are not checked here: the settings carry the
//     file's name (`SimSettings::file`) for whoever joins the line to a
//     spectrometer and can check them;
//   - a number that is not finite or is outside what a line can be:
//     pressures and partial pressures 0 to 1e4 mbar; sizes 1e-6 to 1e9 cc;
//     conductances and pump speeds 0 to 1e9 L/s; outgassing and leaks 0 to
//     1e3 mbar L / s; consumption 0 to 1e6 1/s; sensitivity above 0 to 1e30;
//     noise 0 to 10; ratios 0 to 1e9; memory 0 to 1e30 fA/s, and no more
//     than 1e3 mbar/s through the sensitivity (reported at whichever of the
//     two the file gives, the memory if both); baselines within 1e30 either
//     side of zero. The network's guarantees hold well inside these.

#include <filesystem>

#include "pychron/core/error.hpp"
#include "pychron/sim/sim_system.hpp"

namespace pychron::sim {

// `base` with what `file` says over it. Names are checked against
// `topology`; roles are read from it (a tank's default gas).
Result<SimSettings> load_sim_settings(const std::filesystem::path& file, const SimTopology& topology,
                                      SimSettings base = {});

}  // namespace pychron::sim
