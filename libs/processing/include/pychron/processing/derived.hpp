#pragma once

// What a reduction leaves in a source's cache of ages (age cache spec 4.1,
// 4.4): the browser reads and filters by it without reducing anything.

#include <optional>

#include "pychron/processing/reduced.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

// Nullopt when the reduction is not one to cache: the analysis has no input
// fingerprint (its source keeps none), or `settings` are not the default
// ones, which the fingerprint does not cover. Else the ages in Ma and K/Ca;
// without an age when the reduction gave none.
[[nodiscard]] std::optional<DerivedAge> derived_age(const ReducedAnalysis& reduced, const ReductionSettings& settings);

}  // namespace pychron::processing
