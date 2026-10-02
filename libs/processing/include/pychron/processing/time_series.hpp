#pragma once

// The time-series figure (design section 8.3): stacked panels of quantities
// against time, one graph per graph index, one colour per group, optional fit
// with envelope and per-group statistics.

#include <memory>

#include "pychron/core/error.hpp"
#include "pychron/processing/dataset.hpp"
#include "pychron/processing/options.hpp"
#include "pychron/processing/scene.hpp"

namespace pychron::processing {

class Unit;

// Schema "figure.time_series" with the factory presets "Default",
// "Air monitor", "Blanks", "Unknowns" and "Spectrometer".
const SchemaPtr& time_series_schema();

// `now` (UTC epoch seconds) is used only when x.kind = relative and
// x.origin = now; pass it explicitly to keep the scene reproducible.
Result<Scene> build_time_series(const Dataset& dataset, const Options& options, double now = 0.0);

std::unique_ptr<Unit> make_time_series_unit();

}  // namespace pychron::processing
