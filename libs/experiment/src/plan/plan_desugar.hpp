#pragma once

// v1 -> v2 rewrite (measurement plan v2 spec, section 4.1). Internal to the
// plan loader: resolve_plan calls it after the v1 typed parse, so every v1
// file reaches the rules and the engine as gases of segments.

#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace pychron::experiment::plan::detail {

// One implicit gas built from the v1 sections of `v1`, which must have parsed
// without problems. `catalog` (may be null) names the isotope configured on a
// protected detector that no hop collects. Fails (Config, "<path>: <why>")
// when a hop is left with nothing to collect.
Result<std::vector<Gas>> desugar_v1(const MeasurementPlan& v1, const ISpectrometerCatalog* catalog);

}  // namespace pychron::experiment::plan::detail
