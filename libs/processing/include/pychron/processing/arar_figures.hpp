#pragma once

// The Ar/Ar figures (data browsing and visualization design, section 8; V2):
// ideogram, age spectrum, inverse isochron, and the last two as a pair. Each
// has a schema with factory presets, a pure scene builder and a unit.
//
//   ideogram          probability curve per group with weighted-mean indicator,
//                     analysis-number and value panels (legacy ideogram aux plots)
//   spectrum          steps against cumulative %39ArK, plateau (Fleck/Mahon or
//                     fixed steps), integrated age, value spectra
//   inverse_isochron  36/40 vs 39/40 with error ellipses, York fit, envelope,
//                     trapped 40/36 and isochron age
//   spectrum_isochron the age spectrum and the inverse isochron of the same
//                     analyses, two graphs per graph of the dataset (side by
//                     side, or stacked). The options are both figures':
//                     their own under "spectrum." and "isochron.", the shared
//                     ones (title, fonts, legend, groups, ...) once. With
//                     isochron.exclude_non_plateau the isochron is fitted to
//                     the steps of the plateau the spectrum shows.

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

const SchemaPtr& spectrum_isochron_schema();  // "figure.spectrum_isochron"
Result<Scene> build_spectrum_isochron(const Dataset& dataset, const Options& options);
std::unique_ptr<Unit> make_spectrum_isochron_unit();

}  // namespace pychron::processing
