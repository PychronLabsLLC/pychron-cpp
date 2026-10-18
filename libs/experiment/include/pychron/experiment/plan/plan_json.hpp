#pragma once

// A resolved MeasurementPlan as JSON (measurement plan v2 spec, plan Task 4):
// the golden form the plan fixtures are pinned against, and what a reviewer
// reads to see what a plan file resolves to.
//
// Keys sorted, 2-space indent, trailing newline. Serializes `info`,
// `detectors`, `gases` (every field; enums as the strings a plan file uses),
// `fits`, `conditionals`, `expose` and `hook`; not the v1 sections (they are
// desugared into `gases`). An unset optional is `null`.

#include <string>

#include "pychron/experiment/plan/plan.hpp"

namespace pychron::experiment::plan {

[[nodiscard]] std::string to_json(const MeasurementPlan& plan);

}  // namespace pychron::experiment::plan
