#pragma once

// Duration estimate: arithmetic over the plan, no execution (spec 4.1).

#include "pychron/experiment/model/types.hpp"
#include "pychron/experiment/plan/plan.hpp"

namespace pychron::experiment::plan {

struct DurationOptions {
  Duration peak_center{0};  // per peak-center block; the plan cannot know it
};

// The after_inlet signal segments of every gas: cycles x positions of settle +
// counts x integration; a position whose target equals the current magnet
// position skips its settle. The first position always settles.
Duration main_duration(const MeasurementPlan& plan);

// Every gas in order: before_inlet segments, the equilibration window
// (inlet_delay_s + time_s, or the sniff and whiff readings when they take
// longer), the other after_inlet and the after_main segments; then every
// gas's after_run segments. A peak_center step costs options.peak_center per
// detector and moves the magnet; a delay step its seconds. during_extraction
// segments run with the extraction and add nothing. Magnet position carries
// across segments and gases, so settles are skipped the same way.
Duration estimate_duration(const MeasurementPlan& plan, const DurationOptions& options = {});

}  // namespace pychron::experiment::plan
