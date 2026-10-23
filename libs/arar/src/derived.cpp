#include "pychron/arar/derived.hpp"

#include <cmath>

namespace pychron::processing {

namespace r = pychron::reduction;

std::optional<DerivedValues> derived_age(const ReducedAnalysis& reduced, const ReductionSettings& settings) {
  if (!reduced.analysis || reduced.analysis->input_fingerprint.empty()) return std::nullopt;
  if (settings != ReductionSettings{}) return std::nullopt;
  DerivedValues out;
  out.uuid = reduced.analysis->uuid;
  out.input_fingerprint = reduced.analysis->input_fingerprint;
  out.values.push_back({"age", std::nullopt, "Ma"});
  if (!reduced.arar || !reduced.arar->ages) return out;
  const r::AgeSet& ages = *reduced.arar->ages;
  // An age that is no number is no age: the filter must never match it.
  if (!std::isfinite(ages.age.nominal()) || !std::isfinite(ages.age.std_dev())) return out;
  // The reduction's ages are in its constants' units (E16); the cache speaks Ma.
  const r::AgeUnits units = reduced.constants ? reduced.constants->age_units : r::AgeUnits::Ma;
  const double to_ma = r::age_scale(units, r::AgeUnits::Ma);
  out.values[0].value = ages.age * to_ma;
  // The same for what goes beside it: a row is a number and its error, or is not there.
  const auto finite = [](const r::UFloat& v) { return std::isfinite(v.nominal()) && std::isfinite(v.std_dev()); };
  if (finite(ages.age_w_j_err)) out.values.push_back({"age_w_j", ages.age_w_j_err * to_ma, "Ma"});
  if (reduced.arar->kca && finite(*reduced.arar->kca)) out.values.push_back({"kca", reduced.arar->kca, ""});
  return out;
}

}  // namespace pychron::processing
