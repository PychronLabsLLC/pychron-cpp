#pragma once

#include <string>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/experiment/model/types.hpp"

namespace pychron::experiment {

// "1,3-5;9" -> holes 1 3 4 5 9. Separators: comma, semicolon, whitespace.
// Descending ranges ("5-3") expand descending. Holes are >= 0.
Result<Position> parse_position(std::string_view text);

// Canonical text with runs of consecutive holes collapsed: "1,3-5,9".
std::string format_position(const Position& p);

// One single-hole Position per hole (position-range expansion into runs).
std::vector<Position> split_position(const Position& p);

// Increment helper: shifts every hole by `step`; holes below 0 are an error.
Result<Position> increment_position(const Position& p, int step = 1);

}  // namespace pychron::experiment
