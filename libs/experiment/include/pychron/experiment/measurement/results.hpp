#pragma once

// RunData -> AnalysisRecord pieces (experiment spec 8.2, 8.3). The fits are
// the same pure reduction::fit used live in the UI and by conditionals.

#include <string>
#include <vector>

#include "pychron/experiment/collect/collector.hpp"
#include "pychron/experiment/conditionals/conditional.hpp"
#include "pychron/experiment/plan/plan.hpp"
#include "pychron/experiment/record/types.hpp"

namespace pychron::experiment::measurement {

// Series as float32 traces with t relative to time zero (the epoch when time
// zero was never set); Data::time_zero is seconds since the epoch.
record::Data to_record_data(const collect::RunData& data);

struct FitOutput {
  record::Results results;
  std::vector<std::string> errors;  // series that could not be fitted, with why
};

// Intercepts: one per signal series, fitted with plan.fits.signal on
// x = t - time_zero, keyed by isotope ("Ar40"), or "Ar36:CDD" when the
// isotope was measured on more than one detector. A series with fewer points
// than the fit's parameters is averaged (the recorded fit is "average"). Baselines: one per
// detector over every baseline point on it (block and baseline hops),
// fitted with plan.fits.baseline; value and error are the intercept's.
FitOutput fit_results(const collect::RunData& data, const plan::Fits& fits);

// Record provenance of the conditionals that applied, tripped and failed.
record::InstalledConditional to_record(const Conditional& c);
record::TrippedConditional to_record(const Trip& t);
record::Conditionals to_record_conditionals(const std::vector<Conditional>& installed, const std::vector<Trip>& trips,
                                            const std::vector<ConditionalError>& errors);

}  // namespace pychron::experiment::measurement
