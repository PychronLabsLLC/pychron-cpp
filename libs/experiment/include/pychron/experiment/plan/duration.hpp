#pragma once

// Duration estimate: arithmetic over the plan, no execution (spec 4.1).

#include "pychron/experiment/model/types.hpp"
#include "pychron/experiment/plan/plan.hpp"

namespace pychron::experiment::plan {

struct DurationOptions {
  Duration peak_center{0};  // per peak-center block; the plan cannot know it
};

// cycles x hops of settle + counts x integration; a hop whose target equals
// the current magnet position skips its settle. The first hop always settles.
Duration main_duration(const MeasurementPlan& plan);

// Whole block sequence: peak_center.before, baseline.before, equilibration
// (sniff runs concurrently), main, baseline.after, peak_center.after. Magnet
// position carries across blocks, so settles are skipped the same way.
Duration estimate_duration(const MeasurementPlan& plan, const DurationOptions& options = {});

}  // namespace pychron::experiment::plan
