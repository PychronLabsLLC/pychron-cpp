#pragma once

// Level and package CSV export (sample and package entry spec, section 6,
// export.hpp). The columns are those `elctl entry positions import` reads.

#include <string>
#include <vector>

#include "pychron/persistence/catalog.hpp"

namespace pychron::entry {

// level, position, identifier, sample, project, principal_investigator,
// material, grainsize, weight, packet, note, j, j_err
const std::vector<std::string>& position_columns();

std::string export_level_csv(const persistence::LevelSheet& sheet);
std::string export_package_csv(const std::vector<persistence::LevelSheet>& sheets);

}  // namespace pychron::entry
