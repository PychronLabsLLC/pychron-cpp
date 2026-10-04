#pragma once

// Save PDF of the Packages window (entry spec, section 9.5): the legacy level
// sheet (irradiation_pdf_writer.py:111-289). A summary page (the package,
// its chronology, its levels with holder and projects), then per level the
// holder drawing and a table with a row for every hole.

#include <map>
#include <optional>
#include <vector>

#include <QString>

#include "entry_headers.hpp"

namespace pychron::ui {

struct PackageSheets {
  persistence::IrradiationRow package;
  std::vector<persistence::LevelSheet> levels;                  // by name
  std::map<persistence::Uuid, persistence::HolderValue> holders;  // by holder reference
  std::optional<persistence::ChronologyValue> chronology;
};

// Pages written; an error when the file cannot be written.
Result<int> write_package_pdf(const QString& path, const PackageSheets& sheets);

}  // namespace pychron::ui
