#pragma once

#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/systems/spectrometer/field_table.hpp"
#include "pychron/systems/spectrometer/molecular_weights.hpp"

namespace pychron::spectrometer {

// Reads pychron's mftable.csv / ic_mftable.csv for the one-shot importer.
//
//   iso,H2,H1,AX,L1,L2,CDD            header: isotope column then detectors
//   #,parabolic,parabolic,...         optional per-detector fit row
//   Ar40,5.78,5.89,...                one row per isotope
//
// Blank lines and other '#' lines are skipped. An optional `mass` column is
// used when present; otherwise masses come from `weights`, and an isotope
// missing from both is an Error{Config} (reported, not guessed). The table
// default fit is quadratic (pychron's default); detectors whose fit differs
// get an override. Values are in `axis` units.
Result<FieldTable> read_mftable_csv(std::string_view csv_text, const MolecularWeights& weights,
                                    TableAxis axis = TableAxis::Dac);

}  // namespace pychron::spectrometer
