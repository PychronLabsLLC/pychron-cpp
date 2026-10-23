#pragma once

// The Ar-Ar part of a reduced analysis (domain seam spec 3.2): reduce() run on
// the signals core's reduce minted, so every Ar-Ar value correlates with the
// core stages, and what the group ages need to run the age equation again.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/arar/arar_reduction.hpp"
#include "pychron/core/error.hpp"
#include "pychron/processing/reduced.hpp"
#include "pychron/processing/units.hpp"

namespace pychron::arar {

// The arar_reduce unit's options (what core's ReductionSettings had).
struct ArArSettings {
  reduction::ConstantsPreset preset = reduction::ConstantsPreset::Default;
  bool include_decay_error = false;
  bool use_irradiation_endtime = false;
  friend bool operator==(const ArArSettings&, const ArArSettings&) = default;
};

struct ArArReduced final : processing::DomainReduced {
  reduction::ArArResult arar;
  // What reduce() was given, for group ages (integrated, isochron) that run
  // the age equation again: the constants, J (the same variable the ages
  // used; absent without a flux, or for an unknown without production ratios
  // or chronology) and the flux's lambda_k_total override.
  reduction::ReductionConstants constants;
  std::optional<reduction::UFloat> j;
  std::optional<reduction::Measured> lambda_k_total;
  // Per isotope, aligned with ReducedAnalysis::isotopes: "decay_corrected"
  // (the IC-corrected stage for an isotope reduce() did not decay correct)
  // and "interference_corrected" (only the argon isotopes reduce() was given).
  // (Named so, not `stages`, which is DomainReduced's list of stage names.)
  std::vector<std::map<std::string, reduction::UFloat, std::less<>>> isotope_stages;
  // The key and isotope name of each entry of `isotope_stages`, to find one by label.
  std::vector<std::string> keys, isotopes;
  // Why there is no age although the ratios stand ("no age: no chronology").
  std::string reduction_error;

  std::string_view domain() const override { return "arar"; }
  // `label` is an isotope key ("H1:Ar40") or name ("Ar40": the first isotope with it).
  std::optional<reduction::UFloat> stage(std::string_view label, std::string_view stage) const override;
  // age, age_w_j, age_w_position, F (or f), kca, cak, kcl, clk,
  // radiogenic_yield (or rad40_percent), rad40, k39, j.
  std::optional<reduction::UFloat> named(std::string_view name) const override;
  std::string units(std::string_view name) const override;
  std::vector<std::string> stages() const override { return {"decay_corrected", "interference_corrected"}; }
  std::vector<std::string> names() const override;
  std::string_view error() const override { return reduction_error; }
};

// The Ar-Ar part of `reduced`, or null when it has none (not reduced by
// arar_reduce, or reduced by another domain).
[[nodiscard]] const ArArReduced* reduced_of(const processing::ReducedAnalysis& reduced) noexcept;

// reduce() over the argon isotopes of `reduced` (its signals, not new ones),
// with the flux, production ratios, chronology and constants of its analysis.
// Fails with the reason when there are no values at all: no argon isotope, an
// argon number or a flux error the source does not have ("not reducible: no
// stored ..."), or reduce() refusing the input. An "unknown" without
// production ratios or chronology reduces without ages (and without J):
// ArArReduced::reduction_error says why.
[[nodiscard]] Result<std::shared_ptr<const ArArReduced>> reduce_arar(const processing::ReducedAnalysis& reduced,
                                                                     const ArArSettings& settings);

// What select, reduce and arar_reduce give one analysis: core's reduction with
// the Ar-Ar part attached, or the reason it has none in reduction_error.
[[nodiscard]] processing::ReducedPtr reduce_with_arar(processing::AnalysisPtr analysis, const ArArSettings& settings = {});

// The arar_reduce unit: Dataset in, Dataset out, after core's reduce.
[[nodiscard]] std::unique_ptr<processing::Unit> make_arar_reduce_unit();

}  // namespace pychron::arar
