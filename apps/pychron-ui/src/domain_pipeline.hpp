#pragma once

// Building a pipeline over a domain: the reduction chain every data window puts
// between its select and whatever it computes.

#include <string>

#include "pychron/processing/domain.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::ui {

// Adds one node per unit kind of `domain.reduction_chain()` to `pipeline`: its
// id is the kind and `suffix`, the first is fed by node `from` and each of the
// others by the one before. Gives the id of the last node (`from` for a domain
// with an empty chain).
std::string add_reduction_chain(processing::Pipeline& pipeline, const processing::UnitRegistry& registry,
                                const processing::Domain& domain, const std::string& from,
                                const std::string& suffix = "");

}  // namespace pychron::ui
