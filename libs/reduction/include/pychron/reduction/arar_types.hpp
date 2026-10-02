// Ar-Ar reduction data model (spec 5.1-5.5): measured values, stored rows,
// isotope signals, constants and presets, production ratios, flux.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction {

// ---- 5.1 Values and stored rows -------------------------------------------

struct Measured {
  double value = 0.0, error = 0.0;  // 1 sigma
};

// One payload row (dvc schema 4.1) reduced to what the age needs.
struct StoredValue {
  double value = 0.0, error = 0.0;
  bool use_manual_value = false;
  double manual_value = 0.0;
  bool use_manual_error = false;
  double manual_error = 0.0;
  std::optional<double> modifier_error;  // baseline_value only
};
// manual_* replace value/error when flagged; modifier_error replaces the error
// (dvc/dvc_analysis.py:722-737, :745-746).
Measured resolve(const StoredValue& row) noexcept;

// ---- 5.2 Isotopes ---------------------------------------------------------

enum class ArgonIsotope : std::uint8_t { Ar40, Ar39, Ar38, Ar37, Ar36 };  // ARGON_KEYS order
inline constexpr std::array<ArgonIsotope, 5> kArgonKeys{ArgonIsotope::Ar40, ArgonIsotope::Ar39,
                                                        ArgonIsotope::Ar38, ArgonIsotope::Ar37,
                                                        ArgonIsotope::Ar36};
std::string_view to_string(ArgonIsotope iso) noexcept;
inline constexpr std::size_t index(ArgonIsotope i) noexcept { return static_cast<std::size_t>(i); }

struct IsotopeSignal {
  UFloat intercept;                      // tag "<iso>"
  UFloat baseline;                       // tag "<iso> bs"
  bool include_baseline_error = false;   // intercept_value.include_baseline_error
  UFloat blank;                          // tag "<iso> bk"; exact 0 when none
  bool correct_for_blank = true;         // false for blank/detector_ic/background types
  UFloat ic_factor = 1.0;                // tag "<iso> IC"
  UFloat discrimination = 1.0;
  std::optional<double> deadtime_tau_s;  // absent = off (D4)
};

struct MeasuredSignal {
  Measured intercept, baseline, blank;
  Measured ic_factor{1.0, 0.0};
  bool include_baseline_error = false, correct_for_blank = true;
  std::optional<double> deadtime_tau_s;
};

// Mints fresh variables with the legacy tags (isotope.py:469, :713, :770;
// dvc/dvc_analysis.py:759).
IsotopeSignal make_signal(ArgonIsotope iso, const MeasuredSignal& m);

// Analysis types whose blank is not subtracted (pychron_constants.py:264).
bool corrects_for_blank(std::string_view analysis_type) noexcept;

// ---- 5.3 Constants --------------------------------------------------------

enum class K3739Mode { Normal, Fixed };
enum class AgeUnits { a, ka, Ma, Ga };

struct CosmogenicRatios {
  Measured solar3836, cosmo3836;
};

// Plain record (D1): value-initialised to zero/false/Normal/Ma, no physics
// defaults. Real values come from a reference record or a named preset.
struct ReductionConstants {
  Measured lambda_b, lambda_e;                     // 1/a
  Measured lambda_cl36, lambda_ar37, lambda_ar39;  // 1/day
  Measured atm4036, atm4038;
  K3739Mode k3739_mode = K3739Mode::Normal;
  Measured fixed_k3739;
  double abundance_sensitivity = 0.0;
  bool allow_negative_ca_correction = false;
  bool use_irradiation_endtime = false;
  std::optional<CosmogenicRatios> cosmogenic;  // use_cosmogenic_correction
  bool include_decay_error = false;
  AgeUnits age_units = AgeUnits::Ma;
};

enum class ConstantsPreset {
  Default,            // D2/D5: lab default for new configurations
  Legacy,             // ArArConstants trait defaults (arar_constants.py:28-94)
  LegacyPreferences,  // legacy preference-pane defaults
};
ReductionConstants constants_preset(ConstantsPreset p) noexcept;
std::string_view to_string(ConstantsPreset p) noexcept;  // "default", "legacy", "legacy_preferences"
// lambda_b + lambda_e as independent variables (sigma combines in quadrature).
Measured lambda_k(const ReductionConstants& c) noexcept;

inline constexpr std::string_view kReductionVersion = "arar-1";

// ---- 5.4 Production ratios ------------------------------------------------

struct ProductionRatios {
  Measured k4039, k3839, k3739, ca3937, ca3837, ca3637, cl3638;  // missing = {0, 0}
  std::optional<Measured> ca_k, cl_k;                            // RATIO_KEYS
};
// Keys INTERFERENCE_KEYS + RATIO_KEYS (pychron_constants.py:277-278). Unknown
// key or non-finite value -> ErrorKind::Config (message names the key).
Result<ProductionRatios> production_from_rows(
    const std::map<std::string, Measured, std::less<>>& rows);

struct ProductionVariables {
  UFloat k4039, k3839, k3739, ca3937, ca3837, ca3637, cl3638;  // tags = key names
  std::optional<UFloat> ca_k, cl_k;
  std::array<VariableId, 7> interference_ids() const;  // for E15/E18 exclusion
};
// One fresh variable per ratio, once per analysis (meta_object.py:239-243).
ProductionVariables make_production_variables(const ProductionRatios& p);

// ---- 5.5 Irradiation and flux ---------------------------------------------

struct Dose {
  double power = 0;
  std::int64_t start_utc_s = 0, end_utc_s = 0;
};
struct DecaySegment {
  double power = 0, duration_days = 0, dt_days = 0;
};
struct Irradiation {
  std::vector<DecaySegment> segments;
  double decay_days = 0;
};

struct DecayFactors {
  double df37 = 1.0, df39 = 1.0;
};

struct Flux {  // flux_value (dvc schema 6.1)
  Measured j;
  double position_jerr = 0.0;
  std::optional<Measured> lambda_k_total;  // overrides lambda_b + lambda_e when nonzero
};
UFloat make_j(const Flux& f);  // tag "J" (dvc/meta_repo.py:701)

// ---- 5.6 Reduction input --------------------------------------------------

// One analysis for reduce() (arar_reduction.hpp). Correlation between
// analyses arises only through UFloats the caller shares (a J, a blank);
// constants are minted afresh inside every reduce() call (Q1, D6).
struct ReductionInput {
  std::array<IsotopeSignal, 5> isotopes;   // indexed by ArgonIsotope
  ReductionConstants constants;
  ProductionVariables production;
  Irradiation irradiation;
  std::optional<UFloat> j;                 // absent: no ages (arar_age.py:659-660)
  double position_jerr = 0.0;
  // From Flux; minted once per reduce() call. When truthy (not exactly
  // 0 +- 0) it replaces lambda_b + lambda_e, which may then be zero.
  std::optional<Measured> lambda_k_total;
  std::optional<Measured> fixed_k3739;     // per-analysis override (arar_age.py:68)
};

// ---- 5.6 Diagnostics ------------------------------------------------------

// Raised when a legacy sentinel or quirk applies; the value stays usable
// (spec 5.6, section 7). Never an error.
enum class Diagnostic : std::uint8_t {
  FUndefined,            // k39 == 0; legacy F = 1 +- 0
  YieldUndefined,        // n40 == 0; legacy 0 +- 0
  AgeUndefined,          // 1 + J F <= 0; legacy 0 +- 0
  KCaUndefined,          // ca37 == 0 (kca, cak absent), or ca37 != 0 but kca == 0 (kca kept, cak absent)
  KClUndefined,          // cl38 == 0 (kcl, clk absent), or cl38 != 0 but kcl == 0 (kcl kept, clk absent)
  CaClampedToZero,       // E11 clamp applied
  FixedK3739ZeroCa3937,  // E10 y = 1 fallback
  NonFiniteResult,       // a computed value is NaN/inf
};
// The enumerator name, e.g. "CaClampedToZero" (golden expect_diagnostics).
std::string_view to_string(Diagnostic d) noexcept;

}  // namespace pychron::reduction
