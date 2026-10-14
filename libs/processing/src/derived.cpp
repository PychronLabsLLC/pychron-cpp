#include "pychron/processing/derived.hpp"

#include <cmath>

namespace pychron::processing {

namespace r = pychron::reduction;

std::optional<DerivedAge> derived_age(const ReducedAnalysis& reduced, const ReductionSettings& settings) {
  if (!reduced.analysis || reduced.analysis->input_fingerprint.empty()) return std::nullopt;
  if (settings != ReductionSettings{}) return std::nullopt;
  DerivedAge out;
  out.uuid = reduced.analysis->uuid;
  out.input_fingerprint = reduced.analysis->input_fingerprint;
  if (!reduced.arar || !reduced.arar->ages) return out;
  const r::AgeSet& ages = *reduced.arar->ages;
  // An age that is no number is no age: the filter must never match it.
  if (!std::isfinite(ages.age.nominal()) || !std::isfinite(ages.age.std_dev())) return out;
  // The reduction's ages are in its constants' units (E16); the cache speaks Ma.
  const r::AgeUnits units = reduced.constants ? reduced.constants->age_units : r::AgeUnits::Ma;
  const double to_ma = r::age_scale(units, r::AgeUnits::Ma);
  out.age = ages.age * to_ma;
  out.age_w_j = ages.age_w_j_err * to_ma;
  out.kca = reduced.arar->kca;
  return out;
}

}  // namespace pychron::processing
