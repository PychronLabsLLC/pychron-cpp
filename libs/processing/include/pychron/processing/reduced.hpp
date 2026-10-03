#pragma once

// An analysis with its reduction (design section 7.3): every per-isotope
// correction stage as a UFloat built from the same variables reduce() used,
// so ratios of stages carry correlated errors.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pychron/processing/model.hpp"
#include "pychron/reduction/arar_reduction.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::processing {

enum class Stage {
  Intercept,
  Baseline,
  Blank,
  IcFactor,
  BaselineCorrected,      // intercept - baseline
  BlankCorrected,         // - blank (unless the type is not blank corrected)
  IcCorrected,            // * discrimination * IC; legacy get_intensity
  DecayCorrected,         // Ar37 and Ar39 decay corrected; equals IcCorrected for others
  InterferenceCorrected,  // Ar40 - K40, K39, Ar38, Ar37, atm36 (E14); argon only
};

std::string_view to_string(Stage stage) noexcept;  // "intercept", "bs_corrected", ...
std::optional<Stage> parse_stage(std::string_view text) noexcept;

struct ReductionSettings {
  reduction::ConstantsPreset preset = reduction::ConstantsPreset::Default;
  bool include_decay_error = false;
  bool use_irradiation_endtime = false;
  friend bool operator==(const ReductionSettings&, const ReductionSettings&) = default;
};

struct IsotopeStages {
  std::string key;
  std::map<Stage, reduction::UFloat> values;
};

struct ReducedAnalysis {
  AnalysisPtr analysis;
  std::vector<IsotopeStages> isotopes;  // same order as analysis->isotopes
  // Present when the five argon isotopes reduced (missing ones enter as 0).
  std::optional<reduction::ArArResult> arar;
  std::string reduction_error;  // why `arar` is absent, if it failed
  // What reduce() was given, for group ages (integrated, isochron) that run
  // the age equation again: the constants, J (the same variable the ages
  // used) and the flux's lambda_k_total override.
  std::optional<reduction::ReductionConstants> constants;
  std::optional<reduction::UFloat> j;
  std::optional<reduction::Measured> lambda_k_total;

  const IsotopeStages* find(std::string_view key) const;
  // Stage of an isotope key, or of the first isotope with that name.
  std::optional<reduction::UFloat> stage(std::string_view key, Stage stage) const;
};

using ReducedPtr = std::shared_ptr<const ReducedAnalysis>;

// Never fails: reduction errors are recorded in reduction_error. Ages need a
// flux in the analysis context. An argon isotope whose intercept, baseline,
// blank or IC factor is unknown (Value::known() false), or a flux whose J
// error or lambda_k_total error is NaN, gives no `arar` (and
// no `j`), and reduction_error names what is missing; that isotope's stages
// are NaN.
ReducedPtr reduce_analysis(AnalysisPtr analysis, const ReductionSettings& settings);

}  // namespace pychron::processing
