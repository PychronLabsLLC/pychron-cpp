#pragma once

// RunData -> AnalysisRecord pieces (experiment spec 8.2, 8.3). The fits are
// the same pure reduction::fit used live in the UI and by conditionals.

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::measurement {

// Series as float32 traces with t relative to time zero (the epoch when time
// zero was never set); Data::time_zero is seconds since the epoch. One record
// series per collector series (label, detector, kind, gas, segment, position),
// ordered by label, detector, kind (signal, baseline, sniff, whiff, others by
// name), gas, segment and position; one GasRec per gas measured, in plan
// order.
record::Data to_record_data(const collect::RunData& data, const plan::MeasurementPlan& plan);

struct FitOutput {
  record::Results results;
  std::vector<std::string> errors;  // series that could not be fitted, with why
};

// Intercepts: one per label and detector, keyed "<label>@<detector>", over
// every position the label was read at on that detector (in time order),
// fitted with the plan's signal fit on x = t - time_zero. A series with fewer
// points than the fit's parameters is averaged (the recorded fit is
// "average"). Baselines: one BaselineRec per baseline position per detector,
// fitted with the plan's baseline fit; per signal a BaselineChoice by the
// spec 6.2 rule (`at`, `bracketing_average`, `nearest`; see results.cpp);
// Results::baselines by detector: the choice of the detector's first signal
// (by label), or its first baseline position when it has no signal.
// `spectrometer_reference` positions a plan that names no reference (as the
// engine was given it). Baseline modifiers (spec 3.5): each detector's
// variable evaluated over the intercepts, its model at that value (an
// expression in x, or the weighted least-squares line through a CSV's
// (x, baseline[, error]) points, its text in `model_files` by the plan's
// name), in Results::baseline_modifiers; the baselines are not changed.
using ModelFiles = std::map<std::string, std::string>;
FitOutput fit_results(const collect::RunData& data, const plan::MeasurementPlan& plan,
                      std::string_view spectrometer_reference = {}, const ModelFiles& model_files = {});

// Record provenance of the conditionals that applied, tripped and failed.
record::InstalledConditional to_record(const Conditional& c);
record::TrippedConditional to_record(const Trip& t);
record::Conditionals to_record_conditionals(const std::vector<Conditional>& installed, const std::vector<Trip>& trips,
                                            const std::vector<ConditionalError>& errors);

}  // namespace pychron::experiment::measurement
