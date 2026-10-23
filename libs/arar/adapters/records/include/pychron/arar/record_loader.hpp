#pragma once

// The Ar-Ar extension loader of a records directory (domain seam spec 3.3).
// Records carry no flux or production, so ages need an optional
// references.toml beside them:
//
//   [flux."66001"]                 # by identifier
//   j = 0.0012
//   j_err = 1.2e-6
//   position_jerr = 0.0            # optional
//
//   [production."NM-300"]          # by irradiation; pairs are [value, error]
//   K4039 = [0.0002, 0.00001]     # legacy keys: K4039 K3839 K3739 Ca3937 Ca3837 Ca3637 Cl3638 Ca_K Cl_K
//   Ca3937 = [0.0007, 0.00001]
//
//   [[chronology."NM-300"]]        # by irradiation
//   power = 1.0
//   start = "2025-01-01T00:00:00Z"
//   end = "2025-01-01T10:00:00Z"

#include <filesystem>

#include "pychron/processing/record_source.hpp"

namespace pychron::arar {

// A loader over `references_toml` for one RecordDirectorySource. Its refresh
// reads the file again when its modification time changed (a file that is
// gone is no references: every record is mapped again without them) and
// fails on a file it cannot read or parse, a production whose ratios it
// cannot read or a chronology date it cannot parse. Its load gives every
// analysis an ArArAnalysisData (analysis_data.hpp): the flux of its
// identifier, the production and chronology of its irradiation, each when
// the file has one.
[[nodiscard]] processing::RecordExtensionLoader record_extension_loader(const std::filesystem::path& references_toml);

}  // namespace pychron::arar
