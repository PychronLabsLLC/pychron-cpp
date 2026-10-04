#pragma once

// The legacy meta-repository text formats that entry reads from files a user
// chooses (sample and package entry spec, section 6): irradiation holders and
// chronologies. Std-only; the parsers are those the meta adapter uses.

#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/persistence/model.hpp"

namespace pychron::dvc {

// A holder file ("<shape>,<radius>[,<has hole numbers>]" then one hole per
// line). Hole ordinals count from 0; a hole without an id is numbered by its
// line, counted from 1 after the header.
Result<persistence::HolderValue> parse_holder_text(std::string_view text);

// "power,start,end" lines, the times naive local in `lab_time_zone`.
Result<persistence::ChronologyValue> parse_chronology_text(std::string_view text, std::string_view lab_time_zone);

}  // namespace pychron::dvc
