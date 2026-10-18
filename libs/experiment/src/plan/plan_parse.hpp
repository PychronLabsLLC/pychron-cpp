#pragma once

// Internal to the plan loader, and seen by the plan tests through their
// include path: the resolve without the unbuilt-feature gate, and the gate's
// list (measurement plan v2 plan, Task 5).

#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/plan/plan_loader.hpp"

namespace pychron::experiment::plan::detail {

// resolve_plan without the gate: every schema rule, nothing refused for
// being not yet run. For the tests of the schema while the gate stands.
Result<MeasurementPlan> resolve_plan_ungated(std::string_view effective_toml, const PlanResolvers& resolvers,
                                             std::string_view source = "plan");

// One "<path>: <feature>" per v2 feature of `plan` that the engine does not
// run yet, in plan order. A plan desugared from a v1 file has none.
// resolve_plan refuses each as "<source>: <path>: <feature>: not run by this
// build yet". Each phase of the v2 plan deletes the entries it builds.
std::vector<std::string> unbuilt_features(const MeasurementPlan& plan);

}  // namespace pychron::experiment::plan::detail
