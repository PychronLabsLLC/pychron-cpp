// Ar-Ar reduction public API (spec 6): pure step functions over UFloat.
//
// No I/O, no clocks, no globals except the UFloat id counter and tag table.
// Errors are Result<T> with ErrorKind::Config and a "reduction: " prefix.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction {

struct InterferenceComponents {
  UFloat k37, k38, k39, ca36, ca37, ca38, ca39;
};
struct AtmosphericComponents {
  UFloat atm36, atm38, cl36, cl38;
};
struct CosmogenicComponents {
  UFloat cosmo36, cosmo38, noncosmo36, noncosmo38;
};

// ---- 3.1 Isotope arithmetic (per isotope) ---------------------------------

// E1: I - B when include_baseline_error, else I - nom(B) (the baseline's
// nominal is subtracted, its variable does not propagate).
UFloat baseline_corrected(const IsotopeSignal& s);
// E1-E2: baseline_corrected - Bk when correct_for_blank, else unchanged.
UFloat non_detector_corrected(const IsotopeSignal& s);
// E1-E3: non_detector_corrected * D * IC, in that order. IC == 0 is honoured
// (spec Q18).
UFloat corrected_intensity(const IsotopeSignal& s);

// E4 over ARGON_KEYS order (Ar40, Ar39, Ar38, Ar37, Ar36). The 40 and 36
// neighbours are 2*s39 and 2*s37 (no 41/35; spec Q9).
std::array<UFloat, 5> abundance_sensitivity_correction(const std::array<UFloat, 5>& s,
                                                       double alpha);

// E5 (D4): fA -> counts/s, exact e / 1e-15 C. Legacy deadtime.py:62 used 6240.
inline constexpr double kFaToCountsPerSecond = 6241.509;

// E5: n = s * fa_to_cps; n' = n / (1 - n tau); s' = n' / fa_to_cps. Error
// (ErrorKind::Config, "reduction: deadtime ...") when 1 - n tau <= 0, tau is
// negative or not finite, or fa_to_cps is not finite and positive.
Result<UFloat> deadtime_correct(const UFloat& signal_fa, double tau_s,
                                double fa_to_cps = kFaToCountsPerSecond);

// E5 applied to the signal's intercept (before E1): the intercept itself
// (same variables) when deadtime_tau_s is absent (off by default, D4),
// otherwise deadtime_correct(intercept, *deadtime_tau_s, fa_to_cps).
Result<UFloat> deadtime_corrected_intercept(const IsotopeSignal& s,
                                            double fa_to_cps = kFaToCountsPerSecond);

// ---- 3.2 Decay since irradiation ------------------------------------------

// E6 (Q11/Q12): UTC epoch seconds, no time zone. t_k = (end - start) days,
// dt_k measured from start (or end when use_irradiation_endtime); decay_days
// is from the first dose's start. Doses keep their order.
Irradiation irradiation_from_doses(std::span<const Dose> doses, std::int64_t analysis_utc_s,
                                   bool use_irradiation_endtime);

// E7 (McDougall & Harrison 3.22). Lambdas per day. Error (Config, "reduction: "
// ... "same unit") when |l * max(|t_k|, |dt_k|)| > 50, or naming lambda_ar37 /
// lambda_ar39 when that lambda is 0 with segments present. No segments: {1, 1}.
Result<DecayFactors> decay_factors(double lambda37_per_day, double lambda39_per_day,
                                   std::span<const DecaySegment> segments);

// ---- 3.3 Interference corrections -----------------------------------------

struct InterferenceOptions {
  K3739Mode mode = K3739Mode::Normal;
  // Per-analysis value (arar_age.py:68); forces fixed mode even when mode ==
  // Normal. As in legacy (`not fixed_k3739`), an exact 0 +- 0 counts as unset.
  std::optional<UFloat> fixed_k3739;
  UFloat constants_fixed_k3739;  // used when mode == Fixed and no per-analysis value
  bool allow_negative_ca_correction = false;
};

// E9-E11. Normal mode (E9) unless a per-analysis fixed_k3739 is set or mode is
// Fixed (E10, x = per-analysis value else constants_fixed_k3739). Missing
// ratios are exact 0. Appends FixedK3739ZeroCa3937 when E10 falls back to
// y = 1 (nom(Ca3937) == 0), then CaClampedToZero when the E11 clamp applies
// (!allow_negative_ca_correction and !(nom(ca37) > 0)). The clamp runs after
// k39 and ca39 used the unclamped ca37 (spec Q8); the clamped ca37, ca36 and
// ca38 are exact 0. `diagnostics` may be null.
InterferenceComponents interference_corrections(const UFloat& a39, const UFloat& a37,
                                                const ProductionVariables& p,
                                                const InterferenceOptions& o,
                                                std::vector<Diagnostic>* diagnostics = nullptr);

// ---- 3.4 Atmospheric, chlorine, cosmogenic ------------------------------

// E12. Mints fresh variables from `c` on every call (spec Q1, D6):
// lambda_Cl36 (tag "lambda_Cl36") and r3836 (tag "atm3836"), one variable
// with the nominal and std of atm4036 / atm4038 as legacy re-wraps it. It is
// distinct from E14's trapped_4036.
//   m = Cl3638 lCl decay_days
//   atm36 = (a36 - ca36 - m (a38 - k38 - ca38)) / (1 - m r3836)
//   atm38 = r3836 atm36;  cl38 = a38 - atm38 - k38 - ca38;  cl36 = m cl38
// A missing Cl3638 is exact 0 (atm36 = a36 - ca36, cl36 exact 0). Error
// (Config, "reduction: ... zero divisor") when nom(1 - m r3836) == 0 exactly
// (spec Q16), or when a constant E12 reads is not finite or has a negative
// sigma.
Result<AtmosphericComponents> atmospheric_components(const UFloat& a38, const UFloat& a36,
                                                     const UFloat& k38, const UFloat& ca38,
                                                     const UFloat& ca36, double decay_days,
                                                     const UFloat& cl3638,
                                                     const ReductionConstants& c);

// E13 two-component solar/cosmogenic split of (c36, c38). Mints fresh
// variables "solar3836" (rs) and "cosmo3836" (rc) on every call.
//   rm = c38 / c36;  fs = (rc - rm) / (rc - rs);  fc = 1 - fs
//   noncosmo38 = fs c38;  cosmo38 = c38 - noncosmo38
//   cosmo36 = fc c36;     noncosmo36 = c36 - cosmo36
// Error (Config, "reduction: ... zero divisor") when nom(c36) == 0 or
// nom(rc - rs) == 0 exactly (spec Q16), or when a ratio is invalid.
Result<CosmogenicComponents> cosmogenic_components(const UFloat& c36, const UFloat& c38,
                                                   const CosmogenicRatios& r);

// ---- 3.5 F and radiogenic yield -------------------------------------------

struct FResult {
  std::optional<UFloat> f;                       // absent when nom(k39) == 0 (FUndefined)
  double f_err_wo_irrad = 0.0;                   // E15; 0 when f absent
  UFloat atm40, k40, rad40;
  std::optional<UFloat> radiogenic_yield;        // percent; absent when nom(n40) == 0
  InterferenceComponents interference;
  AtmosphericComponents atmospheric;             // after the cosmogenic split when enabled
  std::optional<CosmogenicComponents> cosmogenic;  // present when constants.cosmogenic
  std::array<UFloat, 5> interference_corrected;  // E14, ARGON_KEYS order
  std::vector<Diagnostic> diagnostics;
};

// E9-E15 in one pass. `n` is in ARGON_KEYS order with Ar37 and Ar39 already
// decay corrected (a37, a39); Ar40, Ar38 and Ar36 are not (spec 3.5 note).
// Mints fresh constant variables on every call (spec Q1): trapped_4036 (E14,
// from constants.atm4036), plus those of E10 (k3739), E12 (lambda_Cl36,
// atm3836) and E13 (solar3836, cosmo3836). `fixed_k3739` is the per-analysis
// override (E10; 0 +- 0 counts as unset, as legacy).
//   atm40 = atm36 T;  k40 = k39 K4039;  rad40 = n40 - atm40 - k40
//   F = rad40 / k39;  yield = rad40 / n40 * 100
//   interference_corrected = {n40 - k40, k39, n38, a37, atm36}
// f_err_wo_irrad is std(F) with the seven interference-ratio variables
// (p.interference_ids()) treated as exact; legacy's second calc_f pass with
// zero-error ratios is identical by linearity (E15).
// Diagnostics, in order: those of E9-E11 (FixedK3739ZeroCa3937,
// CaClampedToZero), FUndefined (nom(k39) == 0, no F = 1 sentinel, D3),
// YieldUndefined (nom(n40) == 0), then NonFiniteResult once when any computed
// nominal or standard deviation is NaN/inf (e.g. the E9 divisor
// 1 - K3739 Ca3937 or the E10 divisor x + y is exactly 0); values are kept.
// Errors (Config, "reduction: "): a non-finite isotope nominal, an invalid
// constant or fixed_k3739, and the E12/E13 zero divisors.
Result<FResult> calculate_f(const std::array<UFloat, 5>& n, double decay_days,
                            const ProductionVariables& p, const ReductionConstants& c,
                            std::optional<Measured> fixed_k3739 = std::nullopt);

// ---- 3.6 Age ----------------------------------------------------------------

// Factor converting an age in `from` units to `to` units: legacy scale_age's
// scalar(current) * targetscalar(target) (a 1, ka 1e3, Ma 1e6, Ga 1e9).
// legacy:processing/arar_constants.py:143-170
double age_scale(AgeUnits from, AgeUnits to) noexcept;

// E16: t = lambda**-1 * ln(1 + J F) * age_scale(a, c.age_units).
// lambda_K is lambda_k_total when it is set and not exactly 0 +- 0 (legacy
// truthiness, `if not lambda_k` / `if lk:`), else lambda_b + lambda_e. It is
// minted afresh on every call as one variable tagged "lambda_k" (sigma of the
// override, or lambda_b and lambda_e in quadrature, which is equivalent for
// every variance and covariance because the two only ever enter as a sum), and
// enters as its nominal only unless c.include_decay_error (spec Q5).
// Errors (Config, "reduction: "): 1 + J F <= 0 or NaN ("1 + J F", spec Q6:
// legacy returns 0 +- 0; reduce() maps it to AgeUndefined), a non-finite J or
// F nominal, a lambda constant that is not finite or has a negative sigma, or
// a zero lambda_K.
Result<UFloat> age_equation(const UFloat& j, const UFloat& f, const ReductionConstants& c,
                            std::optional<Measured> lambda_k_total = std::nullopt);

// E17-E18. All three ages use the same F and are in c.age_units.
struct AgeSet {
  UFloat age;                 // J' = fresh(nom(J), 0) "J_no_err": analytical error only (Q4)
  UFloat age_w_j_err;         // J as supplied ("J"); share one J UFloat to correlate analyses
  UFloat age_w_position_err;  // J'' = fresh(nom(J), position_jerr) "Position"
  double age_err_wo_irrad = 0.0;    // std(age) without the interference-ratio variables (Q13)
  double age_err_wo_j_irrad = 0.0;  // == age_err_wo_irrad: age already has no J error
};

// ---- Whole pipeline (spec 5.6, 6) -------------------------------------------

struct ArArResult {
  DecayFactors decay;
  std::array<UFloat, 5> corrected;  // E1-E5, E4, E8 ("corrected_intensities"), ARGON_KEYS order
  FResult f;
  // Absent without J, without F, when 1 + J F <= 0 (AgeUndefined), or when F
  // is non-finite (NonFiniteResult raised).
  std::optional<AgeSet> ages;
  // E19. kca / kcl absent when ca37 / cl38 is exactly 0; cak / clk also absent
  // when kca / kcl is exactly 0 (KCaUndefined / KClUndefined).
  std::optional<UFloat> kca, cak, kcl, clk;
  // E20 by isotope name ("Ar40".."Ar36"), percent of var(age_w_j_err); present
  // with ages.
  std::map<std::string, double, std::less<>> age_error_components;
  std::vector<Diagnostic> diagnostics;  // union of all steps, in step order
};

// The single-analysis pipeline, in this order (legacy ArArAge.calculate_age,
// arar_age.py:443-689):
//   validate -> per isotope: E5 deadtime on the intercept (only when
//   deadtime_tau_s is set, D4), then E1-E3 corrected_intensity -> E4
//   abundance sensitivity -> E7 decay factors -> E8 (Ar37, Ar39 only) ->
//   calculate_f (E9-E15) -> ages (E16-E18, when j is set and F is finite) ->
//   K/Ca, K/Cl (E19) -> error components (E20).
// Errors (Config, "reduction: "), naming the field: a non-finite value or a
// non-finite or negative sigma anywhere in the input (spec Q7: a NaN
// intercept is an error, never coerced to 0), a negative abundance
// sensitivity or deadtime tau, a zero lambda_b + lambda_e (exempt when a
// truthy lambda_k_total, i.e. not exactly 0 +- 0, is present: it replaces
// lambda_K, E16), a negative position_jerr, plus the step errors (E5
// saturation, E7 guard, E12/E13 zero divisors).
// Diagnostics, in order: calculate_f's (FixedK3739ZeroCa3937,
// CaClampedToZero, FUndefined, YieldUndefined, NonFiniteResult), then
// AgeUndefined, KCaUndefined, KClUndefined, and NonFiniteResult once at the
// end if a later value (corrected, ages, kca, cak, kcl, clk, components) is
// NaN/inf and calculate_f did not already raise it.
Result<ArArResult> reduce(const ReductionInput& in);

namespace detail {

// Internal to reduce() (Task 11); public only so tests can reach it.
// legacy:processing/arar_age.py:658-686 (_set_age_values). Without
// lambda_k_total each variant reads lambda_K afresh, as each legacy
// age_equation call reads arar_constants.lambda_k; with it, the three share
// one "lambda_k" variable (legacy sets it once on the constants,
// dvc/dvc.py:2303-2305). Returns nullopt when 1 + J F <= 0 (reduce raises
// AgeUndefined, spec Q6/D3). Errors as age_equation, plus a negative or
// non-finite position_jerr.
Result<std::optional<AgeSet>> make_age_set(const UFloat& j, double position_jerr,
                                           const UFloat& f, const ReductionConstants& c,
                                           std::optional<Measured> lambda_k_total,
                                           std::span<const VariableId> interference_ids);

}  // namespace detail

}  // namespace pychron::reduction
