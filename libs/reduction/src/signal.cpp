#include "pychron/reduction/signal.hpp"

#include <cmath>
#include <string>

#include "pychron/reduction/detail/arar_kernels.hpp"

namespace pychron::reduction {

Measured resolve(const StoredValue& row) noexcept {
  Measured m{row.value, row.error};
  if (row.use_manual_value) m.value = row.manual_value;
  if (row.use_manual_error) m.error = row.manual_error;
  if (row.modifier_error) m.error = *row.modifier_error;
  return m;
}

IsotopeSignal make_signal(std::string_view label, const MeasuredSignal& m) {
  const std::string name(label);
  IsotopeSignal s;
  s.intercept = UFloat::variable(m.intercept.value, m.intercept.error, name);
  s.baseline = UFloat::variable(m.baseline.value, m.baseline.error, name + " bs");
  s.blank = UFloat::variable(m.blank.value, m.blank.error, name + " bk");
  s.ic_factor = UFloat::variable(m.ic_factor.value, m.ic_factor.error, name + " IC");
  s.include_baseline_error = m.include_baseline_error;
  s.correct_for_blank = m.correct_for_blank;
  s.deadtime_tau_s = m.deadtime_tau_s;
  return s;
}

bool corrects_for_blank(std::string_view t) noexcept {
  for (const std::string_view prefix : {"blank", "detector_ic", "background"}) {
    if (t.starts_with(prefix)) return false;
  }
  return true;
}

// ---- Isotope arithmetic -----------------------------------------------------

// E1. Without include_baseline_error legacy subtracts nominal_value(baseline),
// keeping the intercept's variables and dropping the baseline's.
// legacy:processing/isotope.py:715-736
UFloat baseline_corrected(const IsotopeSignal& s) {
  if (s.include_baseline_error) return s.intercept - s.baseline;
  return s.intercept - kernels::nominal(s.baseline);
}

// E2. The legacy background term (isotope.py:852-853) is always zero on the
// DVC path and is not ported (spec 3.1).
// legacy:processing/isotope.py:848-854
UFloat non_detector_corrected(const IsotopeSignal& s) {
  UFloat v = baseline_corrected(s);
  if (s.correct_for_blank) v = v - s.blank;
  return v;
}

// E3: (nd * D) * IC, the legacy get_disc_corrected_value then get_intensity
// order. An IC factor of 0 is used as is (spec Q18).
// legacy:processing/isotope.py:820-835
UFloat corrected_intensity(const IsotopeSignal& s) {
  return non_detector_corrected(s) * s.discrimination * s.ic_factor;
}

// E5 (D4). legacy:processing/deadtime.py:54-55 (formula); the legacy tool's
// 6240 factor (deadtime.py:62) is replaced by kFaToCountsPerSecond.
Result<UFloat> deadtime_correct(const UFloat& signal_fa, double tau_s, double fa_to_cps) {
  if (!std::isfinite(tau_s) || tau_s < 0.0) {
    return fail(ErrorKind::Config,
                "reduction: deadtime tau must be finite and >= 0, got " + std::to_string(tau_s));
  }
  if (!std::isfinite(fa_to_cps) || !(fa_to_cps > 0.0)) {
    return fail(ErrorKind::Config, "reduction: deadtime fA to counts/s factor must be finite "
                                   "and > 0, got " +
                                       std::to_string(fa_to_cps));
  }
  const double n_tau = signal_fa.nominal() * fa_to_cps * tau_s;
  if (!(1.0 - n_tau > 0.0)) {
    return fail(ErrorKind::Config,
                "reduction: deadtime saturated, 1 - n*tau <= 0 (n*tau = " +
                    std::to_string(n_tau) + ")");
  }
  return kernels::deadtime(signal_fa, tau_s, fa_to_cps);
}

Result<UFloat> deadtime_corrected_intercept(const IsotopeSignal& s, double fa_to_cps) {
  if (!s.deadtime_tau_s) return s.intercept;
  return deadtime_correct(s.intercept, *s.deadtime_tau_s, fa_to_cps);
}

}  // namespace pychron::reduction
