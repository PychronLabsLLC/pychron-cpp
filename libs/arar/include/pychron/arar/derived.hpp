#pragma once

// What a reduction leaves in a source's cache of ages (age cache spec 4.1,
// 4.4): the browser reads and filters by it without reducing anything.

#include <optional>

#include "pychron/arar/reduce.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/source.hpp"

namespace pychron::processing {

// Nullopt when the reduction is not one to cache: the analysis has no input
// fingerprint (its source keeps none), or `settings` are not the default
// ones, which the fingerprint does not cover. Else the Ar-Ar values of its
// ArArReduced: `age` in Ma (valueless when the reduction gave none), then
// `age_w_j` in Ma and `kca` when there is an age and they are numbers.
[[nodiscard]] std::optional<DerivedValues> derived_age(const ReducedAnalysis& reduced,
                                                       const arar::ArArSettings& settings = {});

}  // namespace pychron::processing
