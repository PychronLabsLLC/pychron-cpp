#pragma once

#include <vector>

#include "pychron/core/config/diagnostic.hpp"
#include "pychron/core/config/system_config.hpp"
#include "pychron/core/error.hpp"
#include "pychron/systems/canvas/canvas.hpp"

namespace pychron::canvas {

struct CrossReport {
  std::vector<config::Diagnostic> errors;    // located in the canvas file
  std::vector<config::Diagnostic> warnings;  // located in the system file

  bool ok() const noexcept { return errors.empty(); }
};

// Checks a loaded canvas against its system config (spec section 6):
//  - errors: canvas `valve`/`rough_valve` not a system [[valves]] entry,
//    `manual_valve` not a [[manual_valves]] entry, `pipette` not a
//    [[pipettes]] entry. A `gauge` need not be a [[gauges]] entry: one that
//    is not is drawn for illustration and shows no reading.
//  - warnings: system valves and manual valves not drawn on the canvas
// Switches have no system counterpart yet and are not checked.
CrossReport cross_validate(const Canvas& canvas, const config::SystemConfig& system);

// Result form: warnings on success, a Config error listing every error otherwise.
Result<std::vector<config::Diagnostic>> check_canvas(const Canvas& canvas, const config::SystemConfig& system);

}  // namespace pychron::canvas
