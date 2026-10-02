// Ar-Ar reduction public API (spec 6): pure step functions over UFloat.
//
// No I/O, no clocks, no globals except the UFloat id counter and tag table.
// Errors are Result<T> with ErrorKind::Config and a "reduction: " prefix.
#pragma once

#include <array>
#include <optional>

#include "pychron/core/error.hpp"
#include "pychron/reduction/arar_types.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction {

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

}  // namespace pychron::reduction
