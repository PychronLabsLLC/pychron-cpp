// Ar-Ar reduction public API (spec 6): pure step functions over UFloat.
//
// No I/O, no clocks, no globals except the UFloat id counter and tag table.
// Errors are Result<T> with ErrorKind::Config and a "reduction: " prefix.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "pychron/core/error.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction {

// ---- 5.6 Diagnostics ------------------------------------------------------

// Raised when a legacy sentinel or quirk applies; the value stays usable
// (spec 5.6, section 7). Never an error.
enum class Diagnostic : std::uint8_t {
  FUndefined,            // k39 == 0; legacy F = 1 +- 0
  YieldUndefined,        // n40 == 0; legacy 0 +- 0
  AgeUndefined,          // 1 + J F <= 0; legacy 0 +- 0
  KCaUndefined,          // ca37 == 0; legacy kca = 0
  KClUndefined,          // cl38 == 0; legacy kcl = 0
  CaClampedToZero,       // E11 clamp applied
  FixedK3739ZeroCa3937,  // E10 y = 1 fallback
  NonFiniteResult,       // a computed value is NaN/inf
};
// The enumerator name, e.g. "CaClampedToZero" (golden expect_diagnostics).
std::string_view to_string(Diagnostic d) noexcept;

struct InterferenceComponents {
  UFloat k37, k38, k39, ca36, ca37, ca38, ca39;
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
// ... "same unit") when |l * max(|t_k|, |dt_k|)| > 50. No segments: {1, 1}.
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

}  // namespace pychron::reduction
