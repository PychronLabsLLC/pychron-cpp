#pragma once

// Positions of a package from CSV (the columns of export.hpp; identifier, j
// and j_err are ignored): each row names an existing sample by sample,
// project, principal_investigator, material and grainsize; an empty cell
// matches any value, and a row that matches more than one sample is an error.

#include <map>
#include <string>
#include <vector>

#include "pychron/entry/csv.hpp"
#include "pychron/entry/level_sheet.hpp"
#include "pychron/entry/sample_import.hpp"

namespace pychron::entry {

struct PositionImportResult {
  int applied = 0;
  std::vector<std::string> errors;  // "line N: ..."; nothing is applied when there are any
};

// Applies the rows to `levels` (by level name). A row for a level not in
// `levels`, a position below 1, an unknown sample or a bad number is an error,
// and then no row is applied.
PositionImportResult apply_position_import(const CsvTable& table, const CatalogSnapshot& catalog,
                                           std::map<std::string, LevelSheetEdit>& levels,
                                           const std::vector<std::string>& pi_names_allowed = {});

}  // namespace pychron::entry
