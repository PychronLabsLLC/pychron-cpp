#pragma once

// The Ar/Ar figures (data browsing and visualization design, section 8; V2):
// ideogram, age spectrum and inverse isochron. Each has a schema with factory
// presets, a pure scene builder and a unit.
//
//   ideogram          probability curve per group with weighted-mean indicator,
//                     analysis-number and value panels (legacy ideogram aux plots)
//   spectrum          steps against cumulative %39ArK, plateau (Fleck/Mahon or
//                     fixed steps), integrated age, value spectra
//   inverse_isochron  36/40 vs 39/40 with error ellipses, York fit, envelope,
//                     trapped 40/36 and isochron age

#include <memory>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"

namespace pychron::processing {

class Unit;

const SchemaPtr& ideogram_schema();  // "figure.ideogram"
Result<Scene> build_ideogram(const Dataset& dataset, const Options& options);
std::unique_ptr<Unit> make_ideogram_unit();

const SchemaPtr& spectrum_schema();  // "figure.spectrum"
Result<Scene> build_spectrum(const Dataset& dataset, const Options& options);
std::unique_ptr<Unit> make_spectrum_unit();

const SchemaPtr& isochron_schema();  // "figure.inverse_isochron"
Result<Scene> build_isochron(const Dataset& dataset, const Options& options);
std::unique_ptr<Unit> make_isochron_unit();

}  // namespace pychron::processing
