// Neutral isotope signals (noble gas spec 3.3): measured values, stored rows,
// and the signal corrections of spec 3.1 (E1-E3, E5) for any isotope label.
// Nothing here knows which gas is measured.
#pragma once

#include <optional>
#include <string_view>

#include "pychron/core/error.hpp"
#include "pychron/reduction/ufloat.hpp"

namespace pychron::reduction {

// ---- Values and stored rows -----------------------------------------------

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

// ---- Isotope signals --------------------------------------------------------

struct IsotopeSignal {
  UFloat intercept;                      // tag "<label>"
  UFloat baseline;                       // tag "<label> bs"
  bool include_baseline_error = false;   // intercept_value.include_baseline_error
  UFloat blank;                          // tag "<label> bk"; exact 0 when none
  bool correct_for_blank = true;         // false for blank/detector_ic/background types
  UFloat ic_factor = 1.0;                // tag "<label> IC"
  UFloat discrimination = 1.0;
  std::optional<double> deadtime_tau_s;  // absent = off (D4)
};

struct MeasuredSignal {
  Measured intercept, baseline, blank;
  Measured ic_factor{1.0, 0.0};
  bool include_baseline_error = false, correct_for_blank = true;
  std::optional<double> deadtime_tau_s;
};

// Mints fresh variables tagged "<label>", "<label> bs", "<label> bk" and
// "<label> IC" (legacy tags: isotope.py:469, :713, :770;
// dvc/dvc_analysis.py:759).
IsotopeSignal make_signal(std::string_view label, const MeasuredSignal& m);

// ---- Isotope arithmetic (per isotope) ---------------------------------------

// E1: I - B when include_baseline_error, else I - nom(B) (the baseline's
// nominal is subtracted, its variable does not propagate).
UFloat baseline_corrected(const IsotopeSignal& s);
// E1-E2: baseline_corrected - Bk when correct_for_blank, else unchanged.
UFloat non_detector_corrected(const IsotopeSignal& s);
// E1-E3: non_detector_corrected * D * IC, in that order. IC == 0 is honoured
// (spec Q18).
UFloat corrected_intensity(const IsotopeSignal& s);

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
